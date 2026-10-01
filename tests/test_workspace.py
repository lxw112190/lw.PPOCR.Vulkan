"""GPU workspace growth/rebinding, LRU eviction, bounded rejection and recovery.

Optional --image is a private local regression input: no image or OCR text is
copied into the report. Run alone, not alongside performance/GPU workloads.
"""
import argparse
import ctypes as C
import hashlib
import json
import sys
import time
from pathlib import Path
import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "examples/python"))
from lwvk import load, check, Network, OCR, DeviceInfo


def guarded_run(net, height, width):
    tensor = np.zeros((1, 3, height, width), dtype=np.float32)
    rows, classes = C.c_uint32(), C.c_uint32()
    check(net.lib, net.lib.lwvk_network_shape(net.handle, height, width, C.byref(rows), C.byref(classes)))
    count = rows.value * classes.value
    guarded = np.full(count + 32, -123456, dtype=np.float32)
    out = guarded[16:-16]
    fp = C.POINTER(C.c_float)
    check(net.lib, net.lib.lwvk_network_run(net.handle, tensor.ctypes.data_as(fp), tensor.size,
        height, width, out.ctypes.data_as(fp), count, None))
    assert np.all(guarded[:16] == -123456) and np.all(guarded[-16:] == -123456), "readback overflow"
    assert np.isfinite(out).all()
    return out.copy()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--library", required=True, type=Path)
    p.add_argument("--device", type=int, default=1)
    p.add_argument("--image", type=Path)
    p.add_argument("--report", required=True, type=Path)
    a = p.parse_args()
    lib = load(a.library)
    info = DeviceInfo(struct_size=C.sizeof(DeviceInfo))
    check(lib, lib.lwvk_device_get(a.device, C.byref(info)))
    rows = []
    for variant in ("tiny", "small", "medium"):
        models = ROOT / "models/onnx" / ("ppocrv6-" + variant)
        with Network(lib, models / "det.onnx", a.device) as net:
            baseline = guarded_run(net, 320, 320)
            for height, width in ((960, 736), (736, 960), (960, 960), (64, 96)):
                guarded_run(net, height, width)
            np.testing.assert_allclose(guarded_run(net, 320, 320), baseline, atol=.00001)
        with Network(lib, models / "rec.onnx", a.device) as net:
            baseline = guarded_run(net, 48, 32)
            # More than the 32-plan LRU capacity; alternate probability and CTC.
            for width in range(32, 352, 8):
                guarded_run(net, 48, width)
                net.recognize(np.zeros((1, 3, 48, width), dtype=np.float32))
            guarded_run(net, 48, 960)
            np.testing.assert_allclose(guarded_run(net, 48, 32), baseline, atol=.00001)
        rows.append(dict(model=variant, det_960="passed", rec_growth_ctc_and_40_shapes="passed",
            smaller_readback_canaries="passed"))
        print(variant + ": DET 960 / shared workspace / LRU / readback guards passed", flush=True)
    with Network(lib, ROOT / "models/onnx/ppocrv6-medium/det.onnx", a.device, workspace=128*1024**2) as net:
        baseline = guarded_run(net, 320, 320)
        try:
            guarded_run(net, 960, 960)
        except RuntimeError as e:
            assert "max_workspace_bytes exceeded" in str(e) and "needs" in str(e)
            rejection = str(e)
        else:
            raise AssertionError("explicit low workspace budget was ignored")
        np.testing.assert_allclose(guarded_run(net, 320, 320), baseline, atol=.00001)
    private = None
    if a.image:
        bgr = np.ascontiguousarray(np.asarray(Image.open(a.image).convert("RGB"))[:, :, ::-1])
        timings = []
        with OCR(lib, ROOT / "models/onnx/ppocrv6-medium", a.device, det_limit_side=960) as engine:
            expected = None
            for _ in range(5):
                start = time.perf_counter()
                result = engine.run(bgr)
                timings.append((time.perf_counter()-start)*1000)
                texts = [item["text"] for item in result["items"]]
                if expected is None:
                    expected = texts
                assert texts == expected
        private = dict(image_sha256=hashlib.sha256(a.image.read_bytes()).hexdigest(),
            image_size=[bgr.shape[1], bgr.shape[0]], items=len(expected),
            det_limit_side=960, wall_ms=timings, repeated_results="consistent", text_recorded=False)
        print("Private Medium DET-960 image: " + str(len(expected)) + " items; repeat passed", flush=True)
    report = dict(passed=True, version=lib.lwvk_version().decode(), device=info.name.decode(),
        library_sha256=hashlib.sha256(a.library.read_bytes()).hexdigest(), model_tests=rows,
        low_budget_rejection=rejection, low_budget_recovery="passed", private_image=private,
        note="Functional regression, not a VRAM measurement or leak proof.")
    a.report.parent.mkdir(parents=True, exist_ok=True)
    a.report.write_text(json.dumps(report, ensure_ascii=False, indent=2)+"\n", encoding="utf-8")


if __name__ == "__main__":
    main()
