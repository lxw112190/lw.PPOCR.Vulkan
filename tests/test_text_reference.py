"""CLS and recognition-only GPU probabilities/CTC versus independent ORT CPU."""
import argparse
import ctypes as C
import json
from pathlib import Path
import sys
from concurrent.futures import ThreadPoolExecutor
import numpy as np
import onnxruntime as ort
from PIL import Image
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "examples/python"))
from lwvk import load, Network, DeviceInfo, check

for stream in (sys.stdout, sys.stderr):
    if hasattr(stream, "reconfigure"):
        stream.reconfigure(encoding="utf-8", errors="backslashreplace")


def prepare_crop(image, width):
    h = 48
    resized = min(width, max(1, int(np.ceil(h * image.width / image.height))))
    # Model input BGR. Padding follows lw.PPOCR.C: normalized byte 128.
    value = np.full((1, 3, h, width), 128 * (2 / 255) - 1, dtype=np.float32)
    bgr = np.asarray(image.resize((resized, h), Image.Resampling.BILINEAR), dtype=np.float32)[:, :, ::-1]
    value[0, :, :, :resized] = (bgr * np.float32(2 / 255) - 1).transpose(2, 0, 1)
    return value


def decode(probabilities, dictionary):
    chars = [""] + dictionary + [" "]
    previous, text, scores = -1, [], []
    for row in probabilities:
        label = int(row.argmax())
        if label and label != previous:
            text.append(chars[label]); scores.append(float(row[label]))
        previous = label
    return "".join(text), float(np.mean(scores)) if scores else 0.0


def native_preprocess_reference(bgr, target):
    """Independent NumPy half-pixel bilinear oracle for the native BGR path."""
    h, w = bgr.shape[:2]
    scaled = (48 * w + h - 1) // h
    target = target or min(960, max(32, ((scaled + 7) // 8) * 8))
    actual = min(target, scaled)
    y = (np.arange(48, dtype=np.float64) + .5) * h / 48 - .5
    x = (np.arange(actual, dtype=np.float64) + .5) * w / actual - .5
    y0, x0 = np.floor(y).astype(int), np.floor(x).astype(int)
    fy, fx = (y - y0)[:, None, None], (x - x0)[None, :, None]
    a = bgr[np.clip(y0, 0, h-1)[:, None], np.clip(x0, 0, w-1)[None, :]].astype(np.float64)
    b = bgr[np.clip(y0, 0, h-1)[:, None], np.clip(x0+1, 0, w-1)[None, :]].astype(np.float64)
    c = bgr[np.clip(y0+1, 0, h-1)[:, None], np.clip(x0, 0, w-1)[None, :]].astype(np.float64)
    d = bgr[np.clip(y0+1, 0, h-1)[:, None], np.clip(x0+1, 0, w-1)[None, :]].astype(np.float64)
    top, bottom = a + (b-a)*fx, c + (d-c)*fx
    value = np.full((1, 3, 48, target), 128 * 2/255 - 1, dtype=np.float32)
    value[0, :, :, :actual] = ((top + (bottom-top)*fy)*2/255-1).transpose(2, 0, 1)
    return value


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--library", type=Path, required=True)
    p.add_argument("--device", type=int, default=0)
    p.add_argument("--model-root", type=Path)
    p.add_argument("--iterations", type=int, default=20)
    p.add_argument("--report", type=Path)
    p.add_argument("--quick", action="store_true")
    a = p.parse_args()
    if not 1 <= a.iterations <= 5000:
        p.error("iterations must be 1..5000")
    root = Path(__file__).resolve().parents[1]
    models = a.model_root or root / "models/ppocrv6-tiny"
    direct=(models/"rec.onnx").exists()
    words = (models/("dictionary.txt" if direct else "rec/dictionary.txt")).read_bytes().decode("utf-8").split("\n")[:-1]
    classes=len(words)+2
    lib = load(a.library)
    info = DeviceInfo(struct_size=C.sizeof(DeviceInfo))
    check(lib, lib.lwvk_device_get(a.device, C.byref(info)))
    options = ort.SessionOptions(); options.intra_op_num_threads = 2; options.inter_op_num_threads = 1
    sample = Image.open(root / "test-images/sample.jpg").convert("RGB")
    crops = [sample.crop((20, 28, 312, 74)), sample.crop((22, 237, 248, 264)),
             sample.crop((20, 269, 206, 295)), sample.crop((20, 392, 356, 424))]
    tests, summaries, rss = [], [], []
    import psutil
    process = psutil.Process()
    for task in ("cls", "rec"):
        cpu = ort.InferenceSession(str(models/f"{task}.onnx" if direct else root / f"models/ppocrv6-tiny/{task}/source.onnx"),
            options, providers=["CPUExecutionProvider"])
        if task == "cls":
            inputs = [np.ascontiguousarray((np.asarray(crop.resize((160, 80)), dtype=np.float32)
                [:, :, ::-1] * np.float32(2 / 255) - 1).transpose(2, 0, 1)[None])
                for crop in (crops[0], crops[0].rotate(180))]
        else:
            widths = [32, 96, 320] if a.quick else [32, 64, 96, 320, 640, 960]
            inputs = [prepare_crop(crops[i % len(crops)], width) for i, width in enumerate(widths)]
        with Network(lib, models / (f"{task}.onnx" if direct else f"{task}/model.json"), a.device) as net:
            refs = [cpu.run(None, {cpu.get_inputs()[0].name: t})[0].reshape(-1, 2 if task == "cls" else classes) for t in inputs]
            for t, expected in zip(inputs, refs):
                actual, ms = net.run(t)
                delta = float(np.max(np.abs(actual - expected)))
                if delta > .002 or not np.isfinite(actual).all():
                    raise AssertionError(f"{task} {t.shape}: probability error {delta}")
                np.testing.assert_allclose(actual.sum(axis=1), 1, atol=1e-4)
                if task == "rec":
                    expected_text, expected_score = decode(expected, words)
                    text, score, _ = net.recognize(t)
                    if text != expected_text or abs(score - expected_score) > .002:
                        raise AssertionError(f"REC CTC mismatch: {text!r} vs {expected_text!r}")
                else:
                    text, score = str(int(actual.argmax())), float(actual.max())
                    if int(actual.argmax()) != int(expected.argmax()):
                        raise AssertionError("CLS label mismatch")
                row = dict(task=task, width=t.shape[3], rows=actual.shape[0], max_abs=delta,
                    text=text, score=score, inference_ms=ms)
                summaries.append(row); print(json.dumps(row, ensure_ascii=False), flush=True)
            fp = C.POINTER(C.c_float); t = inputs[0]; output = np.empty_like(refs[0])
            assert lib.lwvk_network_run(net.handle, t.ctypes.data_as(fp), t.size, t.shape[2], t.shape[3],
                output.ctypes.data_as(fp), 1, None) == 5
            nan = t.copy(); nan.flat[0] = np.nan
            assert lib.lwvk_network_run(net.handle, nan.ctypes.data_as(fp), nan.size, t.shape[2], t.shape[3],
                output.ctypes.data_as(fp), output.size, None) == 1
            if task == "rec":
                required, score = C.c_uint64(), C.c_float()
                assert lib.lwvk_recognize_tensor(net.handle, t.ctypes.data_as(fp), t.size, t.shape[3],
                    None, 0, C.byref(required), C.byref(score), None) == 5
                assert required.value >= 1
                # Strided BGR input: padding is not image data, final row may omit padding.
                bgr = np.ascontiguousarray(np.asarray(crops[0])[:, :, ::-1])
                expected_tensor = native_preprocess_reference(bgr, 0)
                expected = cpu.run(None, {cpu.get_inputs()[0].name: expected_tensor})[0].reshape(-1, classes)
                expected_text, expected_score = decode(expected, words)
                text, actual_score, ms = net.recognize_bgr(bgr)
                assert text == expected_text == "纯臻营养护发素"
                assert abs(actual_score - expected_score) < .002
                stride = bgr.shape[1] * 3 + 5
                packed = np.full((bgr.shape[0], stride), 201, dtype=np.uint8)
                packed[:, :bgr.shape[1]*3] = bgr.reshape(bgr.shape[0], -1)
                buffer_bytes = (bgr.shape[0]-1)*stride + bgr.shape[1]*3
                utf8 = C.create_string_buffer(4096)
                score, required = C.c_float(), C.c_uint64()
                def raw_run(size, step):
                    return lib.lwvk_recognize_bgr(net.handle, packed.ctypes.data_as(C.POINTER(C.c_uint8)),
                        size, bgr.shape[1], bgr.shape[0], step, 0, utf8, len(utf8), C.byref(required), C.byref(score), None)
                assert raw_run(buffer_bytes - 1, stride) == 1
                assert raw_run(buffer_bytes, bgr.shape[1]*3-1) == 1
                check(lib, raw_run(buffer_bytes, stride))
                assert utf8.value.decode() == expected_text
                summaries.append(dict(task="rec-bgr", text=text, score=actual_score,
                    cpu_score=expected_score, inference_ms=ms, stride_tests="passed"))
            with ThreadPoolExecutor(max_workers=4) as pool:
                def concurrent(k):
                    k %= len(inputs)
                    actual, _ = net.run(inputs[k])
                    assert float(np.max(np.abs(actual - refs[k]))) < .002
                list(pool.map(concurrent, range(16)))
            for i in range(a.iterations):
                k = (i // 2) % len(inputs)
                actual, _ = net.run(inputs[k])
                if float(np.max(np.abs(actual - refs[k]))) > .002:
                    raise AssertionError(f"{task} repeat {i} inconsistent")
                if i % 50 == 0:
                    rss.append(dict(task=task, iteration=i, rss_bytes=process.memory_info().rss))
                if i % 100 == 0:
                    print(f"{task} repeats {i+1}/{a.iterations}", flush=True)
            tests.append(dict(task=task, iterations=a.iterations, passed=True))
    report = dict(version=lib.lwvk_version().decode(), device=info.name.decode(), reference=summaries,
        repeat_tests=tests, rss_samples=rss, rss_note="Python/ORT/Vulkan driver combined RSS; not leak proof",
        stage="CLS/REC probabilities and recognition-only; not full OCR", passed=True)
    if a.report:
        a.report.parent.mkdir(parents=True, exist_ok=True)
        a.report.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print("PASS: CLS/REC reference, native CTC, changing widths, invalid buffers and recovery")


if __name__ == "__main__":
    main()
