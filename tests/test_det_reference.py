"""Real-model Vulkan versus ONNX Runtime CPU; ORT is a TEST dependency only."""
import argparse
import ctypes as C
import json
import os
from pathlib import Path
import sys
import time
import numpy as np
import onnxruntime as ort
from PIL import Image
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "examples/python"))
from lwvk import load, check, Detector, DeviceInfo


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--library", type=Path, required=True)
    p.add_argument("--device", type=int, default=0)
    p.add_argument("--model", type=Path, help="test assets from an installed package")
    p.add_argument("--iterations", type=int, default=20)
    p.add_argument("--report", type=Path)
    p.add_argument("--quick", action="store_true", help="small shapes for software Vulkan CI")
    a = p.parse_args()
    if not 1 <= a.iterations <= 5000:
        p.error("iterations must be 1..5000")
    root = Path(__file__).resolve().parents[1]
    lib = load(a.library)
    info = DeviceInfo(struct_size=C.sizeof(DeviceInfo))
    check(lib, lib.lwvk_device_get(a.device, C.byref(info)))
    options = ort.SessionOptions()
    options.intra_op_num_threads = 2
    options.inter_op_num_threads = 1
    cpu = ort.InferenceSession(str(root / "models/ppocrv6-tiny/det.onnx"), options, providers=["CPUExecutionProvider"])
    shapes = [(32,32), (64,96), (96,64)] if a.quick else [(32,32), (64,96), (96,64), (192,320), (640,640), (960,640)]
    sample = Image.open(root / "test-images/sample.jpg").convert("RGB")
    tensors = []
    for h, w in shapes:
        # CPU/GPU receive exactly the same bytes: avoids decoder/resize differences.
        rgb = np.asarray(sample.resize((w, h), Image.Resampling.BILINEAR), dtype=np.float32) / 255
        normalized = (rgb - np.array([.485,.456,.406], dtype=np.float32)) / np.array([.229,.224,.225], dtype=np.float32)
        tensors.append(np.ascontiguousarray(normalized.transpose(2,0,1)[None]))
    reference = [cpu.run(None, {cpu.get_inputs()[0].name: t})[0] for t in tensors]
    rows, timings = [], []
    rss = []
    try:
        import psutil
        process = psutil.Process()
    except ImportError:
        process = None
    start = time.time()
    with Detector(lib, a.model or root / "models/ppocrv6-tiny/det.json", a.device) as detector:
        for i, (shape, tensor, expected) in enumerate(zip(shapes, tensors, reference)):
            out, ms = detector.run(tensor)
            delta = np.abs(out - expected)
            if not np.isfinite(out).all() or float(delta.max()) > .001:
                raise AssertionError(f"shape {shape}: max abs error {delta.max()} exceeds 0.001")
            x, y = out >= .2, expected >= .2
            union = np.logical_or(x,y).sum()
            iou = float(np.logical_and(x,y).sum()/union) if union else 1.0
            if iou < .995:
                raise AssertionError(f"shape {shape}: threshold-map IoU {iou} below 0.995")
            row = {"shape": list(shape), "max_abs": float(delta.max()), "mean_abs": float(delta.mean()),
                   "threshold_0_2_iou": iou, "first_run_ms_excluding_plan": ms}
            rows.append(row)
            print(json.dumps(row), flush=True)
        # Same-shape reuse, changing shapes, early rejection, then successful recovery.
        fp = C.POINTER(C.c_float)
        good = tensors[0]
        output = np.empty((32,32),dtype=np.float32)
        assert lib.lwvk_detector_run(detector.handle, good.ctypes.data_as(fp), good.size, 32,32,
                                    output.ctypes.data_as(fp), 1, None) == 5
        nan = good.copy(); nan.flat[0] = np.nan
        assert lib.lwvk_detector_run(detector.handle, nan.ctypes.data_as(fp), nan.size, 32,32,
                                    output.ctypes.data_as(fp), output.size, None) == 1
        assert lib.lwvk_detector_run(detector.handle, good.ctypes.data_as(fp), good.size, 33,32,
                                    output.ctypes.data_as(fp), output.size, None) == 1
        for i in range(a.iterations):
            # Reuse each plan twice before changing the shape; one retained plan only.
            k = (i // 2) % len(tensors)
            out, ms = detector.run(tensors[k])
            if not np.isfinite(out).all() or float(np.max(np.abs(out-reference[k]))) > .001:
                raise AssertionError(f"repeat {i}: inconsistent DET result")
            timings.append(ms)
            if process and i % 10 == 0:
                rss.append({"iteration": i, "rss_bytes": process.memory_info().rss})
            if i % 10 == 0:
                print(f"repeat {i+1}/{a.iterations}, device={info.name.decode()}, inference_ms={ms:.3f}",flush=True)
    report = {"stage": "Tiny DET FP32 only; not complete OCR or release certification",
              "version": lib.lwvk_version().decode(), "device_index": a.device, "device": info.name.decode(),
              "device_type": info.device_type, "pid": os.getpid(), "iterations": a.iterations, "reference": rows,
              "total_wall_s": time.time()-start, "inference_p50_ms": float(np.median(timings)),
              "rss_samples": rss, "rss_note": "observational only; does not prove leak freedom",
              "passed": True}
    if a.report:
        a.report.parent.mkdir(parents=True,exist_ok=True)
        a.report.write_text(json.dumps(report,ensure_ascii=False,indent=2)+"\n",encoding="utf-8")
    print("PASS: full 242-node Tiny DET graph, reference, repeats, invalid input/recovery")
    return 0


if __name__ == "__main__":
    for stream in (sys.stdout,sys.stderr):
        if hasattr(stream,"reconfigure"):
            stream.reconfigure(encoding="utf-8",errors="backslashreplace")
    raise SystemExit(main())
