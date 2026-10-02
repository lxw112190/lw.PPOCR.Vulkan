"""End-to-end Vulkan OCR vs ORT CPU graphs; same reviewed C geometry, independent NumPy preprocessing/CTC.
This is NOT an independent DB/crop algorithm comparison: geometry has host golden unit tests.
"""
import argparse
import ctypes as C
import json
import os
from pathlib import Path
import sys
from concurrent.futures import ThreadPoolExecutor
import numpy as np
import onnxruntime as ort
import psutil
from PIL import Image
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "examples/python"))
from lwvk import load, check, OCR, DeviceInfo
from test_text_reference import decode, native_preprocess_reference
for stream in (sys.stdout, sys.stderr):
    if hasattr(stream, "reconfigure"):
        stream.reconfigure(encoding="utf-8", errors="backslashreplace")


class Box(C.Structure):
    _fields_ = [(v, C.c_float) for v in ("x1", "y1", "x2", "y2", "x3", "y3", "x4", "y4", "score")] + [("reserved", C.c_uint32)]


def resize(bgr, height, width):
    h, w = bgr.shape[:2]
    y = (np.arange(height, dtype=np.float64) + .5) * h / height - .5
    x = (np.arange(width, dtype=np.float64) + .5) * w / width - .5
    y0, x0 = np.floor(y).astype(int), np.floor(x).astype(int)
    fy, fx = (y-y0)[:, None, None], (x-x0)[None, :, None]
    a = bgr[np.clip(y0, 0, h-1)[:, None], np.clip(x0, 0, w-1)[None, :]].astype(np.float64)
    b = bgr[np.clip(y0, 0, h-1)[:, None], np.clip(x0+1, 0, w-1)[None, :]].astype(np.float64)
    c = bgr[np.clip(y0+1, 0, h-1)[:, None], np.clip(x0, 0, w-1)[None, :]].astype(np.float64)
    d = bgr[np.clip(y0+1, 0, h-1)[:, None], np.clip(x0+1, 0, w-1)[None, :]].astype(np.float64)
    return a+(b-a)*fx + ((c+(d-c)*fx)-(a+(b-a)*fx))*fy


def reference(bgr, geometry, sessions, dictionary, classifier=True, limit=960):
    u = C.c_uint32
    f = C.c_float
    fp = C.POINTER(f)
    up = C.POINTER(C.c_uint8)
    geometry.lw_db_postprocess_f32.argtypes = [fp, u, u, f, f, f, u, u, u, u, f, f,
        C.POINTER(Box), u, C.POINTER(u)]
    geometry.lw_sort_detection_boxes.argtypes = [C.POINTER(Box), u, u]
    geometry.lw_crop_quad_size.argtypes = [C.POINTER(Box), C.POINTER(u), C.POINTER(u), C.POINTER(C.c_uint64)]
    geometry.lw_crop_quad_bgr_u8.argtypes = [up, C.c_uint64, u, u, u, C.POINTER(Box), up, C.c_uint64,
        C.POINTER(u), C.POINTER(u), C.POINTER(C.c_uint64)]
    h, w = bgr.shape[:2]
    ratio = min(1., limit/max(w, h))
    rw, rh = [max(32, int(np.floor(v*ratio/32+.5))*32) for v in (w, h)]
    tensor = ((resize(bgr, rh, rw)/255 - [.485, .456, .406]) / [.229, .224, .225]).transpose(2, 0, 1)[None].astype(np.float32)
    def run(task, tensor):
        session = sessions[task]
        return session.run(None, {session.get_inputs()[0].name: tensor})[0]
    prediction = np.ascontiguousarray(run("det", tensor).reshape(rh, rw))
    boxes, count = (Box*1000)(), u()
    assert geometry.lw_db_postprocess_f32(prediction.ctypes.data_as(fp), rw, rh, .3, .6, 1.5, 0,
        1000, w, h, rw/w, rh/h, boxes, 1000, C.byref(count)) == 0
    assert geometry.lw_sort_detection_boxes(boxes, count.value, 0) == 0
    items = []
    for box in boxes[:count.value]:
        cw, ch, n = u(), u(), C.c_uint64()
        assert geometry.lw_crop_quad_size(C.byref(box), C.byref(cw), C.byref(ch), C.byref(n)) == 0
        crop = np.empty(n.value, dtype=np.uint8)
        assert geometry.lw_crop_quad_bgr_u8(bgr.ctypes.data_as(up), bgr.nbytes, w, h, bgr.strides[0],
            C.byref(box), crop.ctypes.data_as(up), crop.nbytes, C.byref(cw), C.byref(ch), C.byref(n)) == 0
        crop = crop.reshape(ch.value, cw.value, 3)
        label, cls_score = -1, 0.
        if classifier:
            window = crop[:, :min(cw.value, ch.value*4)]
            cls_input = (resize(window, 80, 160)*2/255-1).transpose(2, 0, 1)[None].astype(np.float32)
            probabilities = run("cls", cls_input).reshape(-1)
            label, cls_score = int(probabilities.argmax()), float(probabilities.max())
            if label == 1 and cls_score > .9:
                crop = np.ascontiguousarray(crop[::-1, ::-1])
        probabilities = run("rec", native_preprocess_reference(crop, 0)).reshape(-1, len(dictionary)+2)
        text, score = decode(probabilities, dictionary)
        item = {v: float(getattr(box, v)) for v in ("x1", "y1", "x2", "y2", "x3", "y3", "x4", "y4")}
        item.update(text=text, score=score, det_score=float(box.score), cls_label=label, cls_score=cls_score)
        items.append(item)
    return items


def compare(actual, expected):
    assert len(actual["items"]) == len(expected), (len(actual["items"]), len(expected))
    for left, right in zip(actual["items"], expected):
        assert "box" not in left
        assert left["text"] == right["text"], (left["text"], right["text"])
        assert left["cls_label"] == right["cls_label"]
        for key in ("score", "det_score", "cls_score"):
            assert abs(left[key]-right[key]) < .003, (key, left[key], right[key])
        for key in ("x1", "y1", "x2", "y2", "x3", "y3", "x4", "y4"):
            assert abs(left[key]-right[key]) <= 1.1, (key, left[key], right[key])
            assert np.isfinite(left[key]) and 0 <= left[key] <= actual["image_width" if key[0] == "x" else "image_height"]
    assert all(np.isfinite(v) and v >= 0 for v in actual["timing"].values())


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--library", required=True, type=Path)
    p.add_argument("--geometry", required=True, type=Path)
    p.add_argument("--models", type=Path)
    p.add_argument("--device", type=int, default=0)
    p.add_argument("--iterations", type=int, default=20)
    p.add_argument("--quick", action="store_true")
    p.add_argument("--report", type=Path)
    a = p.parse_args()
    if not 1 <= a.iterations <= 5000:
        p.error("iterations must be 1..5000")
    root = Path(__file__).resolve().parents[1]
    models = a.models or root/"models/ppocrv6-tiny"
    lib, geometry = load(a.library), C.CDLL(str(a.geometry.resolve()))
    info = DeviceInfo(struct_size=C.sizeof(DeviceInfo)); check(lib, lib.lwvk_device_get(a.device, C.byref(info)))
    preprocess_environment = {key:os.environ.get(key,'unset (native auto)') for key in
        ('LWVK_GPU_DET_PREPROCESS','LWVK_GPU_TEXT_PREPROCESS','LWVK_GPU_CROP_PREPROCESS')}
    software_timeout = os.environ.get('LWVK_SOFTWARE_GRAPH_TIMEOUT_MS','unset (native 30000)')
    print(json.dumps(dict(phase='reference-start',device=bytes(info.name).decode('utf-8','replace'),
        models=str(models),quick=a.quick,preprocessing_environment=preprocess_environment,
        software_graph_timeout_ms=software_timeout)),flush=True)
    options = ort.SessionOptions(); options.intra_op_num_threads=2; options.inter_op_num_threads=1
    direct=(models/"rec.onnx").exists()
    source=models if direct else root/"models/ppocrv6-tiny"
    files=(("det","det.onnx"),("cls","cls.onnx" if direct else "cls/source.onnx"),("rec","rec.onnx" if direct else "rec/source.onnx"))
    sessions = {task: ort.InferenceSession(str(source/file), options,
        providers=["CPUExecutionProvider"]) for task,file in files}
    dictionary = (models/("dictionary.txt" if direct else "rec/dictionary.txt")).read_bytes().decode("utf-8").split("\n")[:-1]
    image = Image.open(root/"test-images/sample.jpg").convert("RGB")
    if a.quick:
        image = image.resize((320, 320))
    variants = [("sample", image), ("rotated-180", image.rotate(180))]
    if not a.quick:
        variants += [("wide-resized", image.resize((640, 400))), ("small", image.resize((320, 320)))]
    variants += [("blank", Image.new("RGB", (96, 64), "white"))]
    inputs = [np.ascontiguousarray(np.asarray(i)[:, :, ::-1]) for _, i in variants]
    expected = [reference(i, geometry, sessions, dictionary) for i in inputs]
    assert any(i["text"] == "纯臻营养护发素" for i in expected[0]) and not expected[-1]
    rows, rss = [], []
    process = psutil.Process()
    rss_before_engine = process.memory_info().rss/1024**2
    with OCR(lib, models, a.device) as engine:
        for (name, _), value, wanted in zip(variants, inputs, expected):
            print(json.dumps(dict(phase='ocr-start',variant=name,image_hw=list(value.shape[:2]),
                expected_regions=len(wanted))),flush=True)
            actual = engine.run(value); compare(actual, wanted)
            rows.append(dict(variant=name, items=len(wanted), texts=[x["text"] for x in actual["items"]], timing=actual["timing"]))
            print(json.dumps(rows[-1], ensure_ascii=False), flush=True)
        # Strided buffer; final-row padding is not required. Rejection must clear output.
        value = inputs[0]; h, w = value.shape[:2]; stride=w*3+7
        packed = np.full((h, stride), 231, dtype=np.uint8); packed[:, :w*3] = value.reshape(h, -1)
        result = C.c_void_p(123); up=C.POINTER(C.c_uint8)
        def raw(size, step):
            return lib.lwvk_ocr_run_bgr(engine.handle, packed.ctypes.data_as(up), size, w, h, step, C.byref(result))
        needed=(h-1)*stride+w*3
        assert raw(needed-1, stride) == 1 and not result.value
        assert raw(needed, w*3-1) == 1 and not result.value
        assert lib.lwvk_ocr_run_bgr(engine.handle, value.ctypes.data_as(up), value.nbytes, 20001, h, w*3, C.byref(result)) == 1
        check(lib, raw(needed, stride))
        size=C.c_uint64(); assert lib.lwvk_ocr_result_json(result, None, 0, C.byref(size)) == 5 and size.value > 1
        tiny=C.create_string_buffer(2)
        assert lib.lwvk_ocr_result_json(result, tiny, 2, C.byref(size)) == 5 and tiny.value == b""
        text=C.create_string_buffer(size.value)
        check(lib, lib.lwvk_ocr_result_json(result, text, len(text), C.byref(size)))
        compare(json.loads(text.value), expected[0])
        for i in range(a.iterations):
            k=(i//2)%len(inputs)
            compare(engine.run(inputs[k]), expected[k])
            if i%10==0 or i==a.iterations-1:
                rss.append(dict(iteration=i+1, rss_mib=process.memory_info().rss/1024**2))
        with ThreadPoolExecutor(max_workers=4) as pool:
            def concurrent(k):
                k %= len(inputs)
                compare(engine.run(inputs[k]), expected[k])
            list(pool.map(concurrent, range(4 if a.quick else 8)))
    # Result lifetime is independent of the engine. Copy doesn't re-infer.
    rss_after_engine = process.memory_info().rss/1024**2
    check(lib, lib.lwvk_ocr_result_json(result, text, len(text), C.byref(size)))
    compare(json.loads(text.value), expected[0]); lib.lwvk_ocr_result_destroy(result)
    with OCR(lib, models, a.device, enable_classifier=0) as engine:
        compare(engine.run(inputs[0]), reference(inputs[0], geometry, sessions, dictionary, False))
    with OCR(lib, models, a.device, max_crop_pixels=1, max_total_crop_pixels=1) as engine:
        try:
            engine.run(inputs[0])
        except RuntimeError as e:
            assert "crop pixel limit" in str(e)
        else:
            raise AssertionError("crop limit wasn't enforced")
        assert not engine.run(inputs[-1])["items"]  # recovery after input rejection
    # Every individual sample crop fits 20k, but the accumulated crop work does not.
    with OCR(lib, models, a.device, max_crop_pixels=20000, max_total_crop_pixels=20000) as engine:
        try:
            engine.run(inputs[0])
        except RuntimeError as e:
            assert "crop pixel limit" in str(e)
        else:
            raise AssertionError("cumulative crop limit wasn't enforced")
        assert not engine.run(inputs[-1])["items"]
    report=dict(version=lib.lwvk_version().decode(), device=bytes(info.name).decode("utf-8", "replace"),
        reference="ORT CPU graphs + independent NumPy preprocess/CTC; shared C geometry with host golden tests",
        comparisons=rows, iterations=a.iterations, rss=rss,
        preprocessing_environment=preprocess_environment,
        software_graph_timeout_ms=software_timeout,
        rss_before_engine_mib=rss_before_engine, rss_after_engine_destroy_mib=rss_after_engine,
        limitations="Process RSS includes ORT/Python/driver, not VRAM or proof of no leaks.",
        input_recovery="passed", result_lifetime="passed", classifier_disabled="passed",
        concurrency=f"4 workers / {4 if a.quick else 8} calls passed")
    if a.report:
        a.report.parent.mkdir(parents=True, exist_ok=True)
        a.report.write_text(json.dumps(report, ensure_ascii=False, indent=2)+"\n", encoding="utf-8")
    print("PASS: full OCR reference/length/stride/resource cap/recovery/result lifetime/concurrency", flush=True)
    return 0
if __name__ == "__main__":
    raise SystemExit(main())
