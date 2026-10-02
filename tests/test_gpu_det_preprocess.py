"""CPU/GPU DET preprocessing paired OCR regression; set experiment BEFORE loading DLLs.

Private image contents are not retained. Corpus timings include JSON copy/parsing,
exclude image decoding, and alternate order. This is not a GPU leak proof.
"""
import argparse
import ctypes as C
import hashlib
import json
import os
from pathlib import Path
import statistics
import sys
import time
import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "examples/python"))
from lwvk import OCR, load, check


def raw(engine, pixels, width, height, stride, length):
    result = C.c_void_p(123)
    status = engine.lib.lwvk_ocr_run_bgr(engine.handle, pixels.ctypes.data_as(C.POINTER(C.c_uint8)),
        length, width, height, stride, C.byref(result))
    if status:
        assert not result.value, "rejected request did not clear result"
        check(engine.lib, status)
    try:
        size = C.c_uint64()
        assert engine.lib.lwvk_ocr_result_json(result, None, 0, C.byref(size)) == 5
        text = C.create_string_buffer(size.value)
        check(engine.lib, engine.lib.lwvk_ocr_result_json(result, text, len(text), C.byref(size)))
        return json.loads(text.value)
    finally:
        engine.lib.lwvk_ocr_result_destroy(result)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--before', type=Path, required=True)
    p.add_argument('--after', type=Path, required=True)
    p.add_argument('--device', type=int, default=1)
    p.add_argument('--corpus', type=Path)
    p.add_argument('--repeats', type=int, default=2)
    p.add_argument('--report', type=Path, required=True)
    a = p.parse_args()
    if os.environ.get('LWVK_GPU_DET_PREPROCESS') != '1':
        p.error('set LWVK_GPU_DET_PREPROCESS=1 before starting this process')
    if any(os.environ.get(n) == '1' for n in ('LWVK_GPU_PROFILE', 'LWVK_HOST_PROFILE')):
        p.error('disable diagnostic profiling')
    if not 1 <= a.repeats <= 10:
        p.error('repeats must be 1..10')
    libraries = [load(a.before), load(a.after)]
    image = Image.open(ROOT / 'test-images/sample.jpg').convert('RGB')
    variants = [image.resize(size) for size in ((32, 32), (320, 320), (500, 500), (501, 503),
        (640, 400), (1000, 1000), (1280, 720), (2448, 3264))]
    variants += [image.rotate(180), Image.new('RGB', (96, 64), 'white')]
    corpus = sorted(a.corpus.glob('img-*.jpg')) if a.corpus else []
    if a.corpus and len(corpus) != 100:
        p.error('expected exactly 100 generated corpus images')
    rows = []
    for model in ('tiny', 'small', 'medium'):
        root = ROOT / 'models/onnx' / ('ppocrv6-' + model)
        engines = [OCR(lib, root, a.device) for lib in libraries]
        try:
            for variant in variants:
                pixels = np.ascontiguousarray(np.asarray(variant)[:, :, ::-1])
                expected = engines[0].run(pixels)['items']
                assert engines[1].run(pixels)['items'] == expected
                h, w = pixels.shape[:2]
                stride = w * 3 + 7
                packed = np.full((h, stride), 231, dtype=np.uint8)
                packed[:, :w * 3] = pixels.reshape(h, -1)
                length = (h - 1) * stride + w * 3
                assert raw(engines[1], packed, w, h, stride, length)['items'] == expected
                for size, step in ((length - 1, stride), (length, w * 3 - 1)):
                    try:
                        raw(engines[1], packed, w, h, step, size)
                    except RuntimeError as e:
                        assert 'invalid BGR' in str(e)
                    else:
                        raise AssertionError('invalid input accepted')
            timing = [[], []]
            digest = hashlib.sha256()
            for index, path in enumerate(corpus):
                with Image.open(path) as im:
                    pixels = np.ascontiguousarray(np.asarray(im.convert('RGB'))[:, :, ::-1])
                expected = None
                for repeat in range(a.repeats + 1):
                    for which in ((0, 1) if (index + repeat) % 2 == 0 else (1, 0)):
                        start = time.perf_counter()
                        items = engines[which].run(pixels)['items']
                        elapsed = (time.perf_counter() - start) * 1000
                        if expected is None:
                            expected = items
                        assert items == expected, f'{model}/{path.name}: items differ'
                        if repeat:
                            timing[which].append(elapsed)
                digest.update(json.dumps(expected, sort_keys=True).encode())
            means = [statistics.mean(x) for x in timing] if corpus else []
            row = dict(model=model, variant_pairs=len(variants), padded_stride='exact items',
                invalid_input='rejected with cleared result', corpus_images=len(corpus),
                corpus_exact_items=True, corpus_items_sha256=digest.hexdigest(),
                before_after_mean_wall_ms=means, measured_samples_per_path=len(timing[0]),
                reduction_percent=100 * (1 - means[1] / means[0]) if corpus else None)
            rows.append(row)
            print(json.dumps(row), flush=True)
        finally:
            for engine in engines:
                engine.close()
    with OCR(libraries[1], ROOT / 'models/onnx/ppocrv6-tiny', a.device) as engine:
        blank = np.full((64, 64, 3), 255, dtype=np.uint8)
        expected = engine.run(blank)['items']
        for i in range(40):
            assert not engine.run(np.full((32 * (1 + i // 20), 32 * (1 + i % 20), 3), 255, dtype=np.uint8))['items']
        assert engine.run(blank)['items'] == expected
    with OCR(libraries[1], ROOT / 'models/onnx/ppocrv6-tiny', a.device,
             det_limit_side=32, max_workspace_bytes=2 * 1024**2) as engine:
        blank = np.full((64, 64, 3), 255, dtype=np.uint8)
        assert not engine.run(blank)['items']
        try:
            engine.run(np.full((1000, 1000, 3), 255, dtype=np.uint8))
        except RuntimeError as e:
            assert 'max_workspace_bytes exceeded' in str(e)
            assert ('shared image/crops' if os.environ.get('LWVK_GPU_CROP_PREPROCESS')=='1' else 'raw BGR upload') in str(e)
        else:
            raise AssertionError('raw upload budget ignored')
        assert not engine.run(blank)['items']
    report = dict(passed=True, device=a.device, gpu_det_preprocess=True, rows=rows,
        gpu_text_preprocess=os.environ.get('LWVK_GPU_TEXT_PREPROCESS')=='1',
        gpu_crop_preprocess=os.environ.get('LWVK_GPU_CROP_PREPROCESS')=='1',
        before_sha256=hashlib.sha256(a.before.read_bytes()).hexdigest(),
        after_sha256=hashlib.sha256(a.after.read_bytes()).hexdigest(),
        lru_40_shapes='passed', raw_upload_budget_and_recovery='passed',
        method='one shape warmup, alternating order, FP32 networks, DET cap 960, classifier enabled',
        limitations='No Vulkan validation-layer claim, long soak/leak proof or cross-device equivalence claim.')
    a.report.parent.mkdir(parents=True, exist_ok=True)
    a.report.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print('PASS: GPU DET preprocessing / 40-plan LRU / upload budget / recovery', flush=True)


if __name__ == '__main__':
    main()
