"""Same-device, same-input FP32 probability-output graph benchmark, not full OCR.

DirectML helper is test-only. GPU indices are independent: match hardware IDs
and require the requested device name. CPU orchestration and output transfer
are included; image preprocessing, DB/crop and CTC are not included.
"""
import argparse
import ctypes as C
import hashlib
import json
import os
import statistics
import sys
import time
from pathlib import Path
import numpy as np
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "examples/python"))
from lwvk import load, check, Network, DeviceInfo


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--library", type=Path, required=True)
    p.add_argument("--dml-library", type=Path, required=True)
    p.add_argument("--device", type=int, default=1)
    p.add_argument("--require-device", default="RTX 4060")
    p.add_argument("--iterations", type=int, default=20)
    p.add_argument("--report", type=Path, required=True)
    a = p.parse_args()
    if os.environ.get("LWVK_GPU_PROFILE")=="1":
        p.error("LWVK_GPU_PROFILE=1 perturbs execution; disable it for performance comparisons")
    if not 1 <= a.iterations <= 5000:
        p.error("iterations must be 1..5000")
    vk = load(a.library)
    dml = C.CDLL(str(a.dml_library.resolve()))
    dml.lwvk_dml_error.restype = dml.lwvk_dml_version.restype = C.c_char_p
    dml.lwvk_dml_adapter.argtypes = [C.c_uint32, C.c_void_p, C.c_uint32, C.POINTER(C.c_uint32), C.POINTER(C.c_uint32)]
    dml.lwvk_dml_create.argtypes = [C.c_char_p, C.c_int, C.POINTER(C.c_void_p)]
    dml.lwvk_dml_destroy.argtypes = [C.c_void_p]
    fp = C.POINTER(C.c_float)
    dml.lwvk_dml_run.argtypes = [C.c_void_p, fp, C.c_uint64, C.c_uint32, C.c_uint32, fp, C.c_uint64]
    def checked(status):
        if status:
            raise RuntimeError(dml.lwvk_dml_error().decode())
    info = DeviceInfo(struct_size=C.sizeof(DeviceInfo))
    check(vk, vk.lwvk_device_get(a.device, C.byref(info)))
    assert a.require_device in info.name.decode(), "wrong Vulkan hardware"
    selected = None
    for index in range(16):
        name, vendor, device = C.create_string_buffer(256), C.c_uint32(), C.c_uint32()
        if dml.lwvk_dml_adapter(index, name, len(name), C.byref(vendor), C.byref(device)):
            break
        if vendor.value == info.vendor_id and device.value == info.device_id and a.require_device in name.value.decode():
            selected = (index, name.value.decode())
            break
    assert selected is not None, "matching DXGI hardware not found; do not assume GPU indices match"
    rows = []
    rng = np.random.default_rng(20260930)
    for variant in ("tiny", "small", "medium"):
        models = ROOT / "models/onnx" / ("ppocrv6-" + variant)
        for task, height, width in (("det", 512, 512), ("det", 960, 736), ("cls", 80, 160), ("rec", 48, 320), ("rec", 48, 960)):
            file = models / (task + ".onnx")
            tensor = rng.uniform(-1, 1, (1, 3, height, width)).astype(np.float32)
            handle = C.c_void_p()
            checked(dml.lwvk_dml_create(str(file.resolve()).encode(), selected[0], C.byref(handle)))
            try:
                with Network(vk, file, a.device) as net:
                    expected, _ = net.run(tensor)
                    actual = np.empty_like(expected)
                    vk_output = np.empty_like(expected)
                    def run_dml():
                        checked(dml.lwvk_dml_run(handle, tensor.ctypes.data_as(fp), tensor.size, height, width,
                            actual.ctypes.data_as(fp), actual.size))
                    def run_vk():
                        check(vk, vk.lwvk_network_run(net.handle, tensor.ctypes.data_as(fp), tensor.size, height, width,
                            vk_output.ctypes.data_as(fp), vk_output.size, None))
                    for _ in range(3):
                        run_dml(); run_vk()
                    delta = float(np.max(np.abs(actual-expected)))
                    assert np.isfinite(actual).all() and delta < .002, (variant, task, delta)
                    measurements = {"vulkan_ms": [], "dml_ms": []}
                    # Alternating order reduces clock/temperature order bias.
                    for i in range(a.iterations):
                        calls = [("vulkan_ms", run_vk), ("dml_ms", run_dml)]
                        for key, call in calls if i % 2 == 0 else calls[::-1]:
                            start = time.perf_counter(); call()
                            measurements[key].append((time.perf_counter()-start)*1000)
                    row = dict(model=variant, task=task, shape=list(tensor.shape), max_abs=delta,
                        model_sha256=hashlib.sha256(file.read_bytes()).hexdigest(), samples=measurements,
                        median_ms={k: statistics.median(v) for k, v in measurements.items()},
                        p95_ms={k: float(np.percentile(v, 95)) for k, v in measurements.items()})
                    rows.append(row)
                    print(json.dumps({k: row[k] for k in ("model", "task", "shape", "median_ms", "max_abs")}), flush=True)
            finally:
                dml.lwvk_dml_destroy(handle)
    report = dict(vulkan_version=vk.lwvk_version().decode(), ort_version=dml.lwvk_dml_version().decode(),
        vulkan_gpu=info.name.decode(), vulkan_index=a.device, dxgi_gpu=selected[1], dxgi_index=selected[0],
        library_sha256=hashlib.sha256(a.library.read_bytes()).hexdigest(),
        dml_helper_sha256=hashlib.sha256(a.dml_library.read_bytes()).hexdigest(),
        onnxruntime_sha256=hashlib.sha256((a.dml_library.parent/"onnxruntime.dll").read_bytes()).hexdigest(),
        directml_sha256=hashlib.sha256((a.dml_library.parent/"DirectML.dll").read_bytes()).hexdigest(),
        method=dict(warmup=3, iterations=a.iterations, precision="FP32 graph inputs/outputs", seed=20260930,
            output="full probabilities copied to CPU for both backends", input="same preprocessed tensor",
            scope="graph-only, not full OCR; ORT DML may retain CPU shape/unsupported operators"), comparisons=rows)
    a.report.parent.mkdir(parents=True, exist_ok=True)
    a.report.write_text(json.dumps(report, indent=2)+"\n", encoding="utf-8")


if __name__ == "__main__":
    main()
