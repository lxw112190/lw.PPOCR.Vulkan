"""GPU REC-only exact old/new ABI regression; run with GPU text flag set before loading.

Covers adaptive/explicit widths, right padding, padded stride, result-length
queries, rejection/recovery, >32 cached shapes and concurrent calls.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import ctypes as C
import hashlib
import json
import os
from pathlib import Path
import sys
import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'examples/python'))
from lwvk import load, Network


def call(net, pixels, w, h, stride, length, target=0, capacity=4096):
    text = C.create_string_buffer(max(capacity, 1))
    text[0] = b'x'
    required, score, elapsed = C.c_uint64(123), C.c_float(-1), C.c_double(-1)
    status = net.lib.lwvk_recognize_bgr(net.handle, pixels.ctypes.data_as(C.POINTER(C.c_uint8)),
        length, w, h, stride, target, text if capacity else None, capacity,
        C.byref(required), C.byref(score), C.byref(elapsed))
    return status, text.value if capacity else b'', required.value, score.value, elapsed.value


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--before', type=Path, required=True)
    p.add_argument('--after', type=Path, required=True)
    p.add_argument('--device', type=int, default=1)
    p.add_argument('--report', type=Path, required=True)
    a = p.parse_args()
    if os.environ.get('LWVK_GPU_TEXT_PREPROCESS') != '1':
        p.error('set LWVK_GPU_TEXT_PREPROCESS=1 before launching')
    libraries = [load(a.before), load(a.after)]
    with Image.open(ROOT/'test-images/sample.jpg') as im:
        crop = im.convert('RGB').crop((20, 28, 312, 74))
    images = [crop, crop.rotate(180), crop.resize((13, 97)), crop.resize((1600, 23)),
              crop.resize((97, 25)), Image.new('RGB', (1, 1), 'white')]
    rows = []
    for model in ('tiny', 'small', 'medium'):
        nets = [Network(lib, ROOT/'models/onnx'/('ppocrv6-'+model)/'rec.onnx', a.device) for lib in libraries]
        try:
            count = 0
            for image in images:
                pixels = np.ascontiguousarray(np.asarray(image)[:, :, ::-1]); h, w = pixels.shape[:2]
                stride = w*3+7
                padded = np.full((h, stride), 211, dtype=np.uint8)
                padded[:, :w*3] = pixels.reshape(h, -1)
                length = (h-1)*stride+w*3
                for target in (0, 32, 320, 960):
                    old = call(nets[0], padded, w, h, stride, length, target)
                    new = call(nets[1], padded, w, h, stride, length, target)
                    assert old[:4] == new[:4] and new[0] == 0, (model, image.size, target, old[:4], new[:4])
                    for capacity in (0, 1, new[2]-1, new[2]):
                        old = call(nets[0], padded, w, h, stride, length, target, capacity)
                        new = call(nets[1], padded, w, h, stride, length, target, capacity)
                        assert old[:4] == new[:4], (model, target, capacity)
                    count += 1
                for size, step, target in ((length-1, stride, 0), (length, w*3-1, 0), (length, stride, 33)):
                    failed = call(nets[1], padded, w, h, step, size, target)
                    assert failed[:4] == (1, b'', 0, 0.0), failed[:4]
                assert nets[0].recognize_bgr(pixels)[:2] == nets[1].recognize_bgr(pixels)[:2]
            pixels = np.ascontiguousarray(np.asarray(crop)[:, :, ::-1])
            wanted = nets[1].recognize_bgr(pixels, 32)[:2]
            for width in range(32, 352, 8):
                assert nets[0].recognize_bgr(pixels, width)[:2] == nets[1].recognize_bgr(pixels, width)[:2]
            assert nets[1].recognize_bgr(pixels, 32)[:2] == wanted
            with ThreadPoolExecutor(max_workers=4) as pool:
                assert all(result == wanted for result in pool.map(lambda _: nets[1].recognize_bgr(pixels, 32)[:2], range(20)))
            row = dict(model=model, exact_rec_bgr_cases=count, length_query_and_small_buffer='passed',
                       padded_stride_and_invalid_input_recovery='passed', lru_40_widths='passed', concurrent_calls=20)
            rows.append(row); print(json.dumps(row), flush=True)
        finally:
            for net in nets: net.close()
    # Raw upload must be budgeted even when target REC tensor is very narrow.
    with Network(libraries[1], ROOT/'models/onnx/ppocrv6-tiny/rec.onnx', a.device, workspace=2*1024**2) as net:
        small = np.full((48, 32, 3), 128, dtype=np.uint8)
        expected = net.recognize_bgr(small, 32)[:2]
        large = np.full((1000, 1000, 3), 128, dtype=np.uint8)
        try:
            net.recognize_bgr(large, 32)
        except RuntimeError as e:
            assert 'max_workspace_bytes exceeded' in str(e) and 'raw BGR upload' in str(e)
        else:
            raise AssertionError('REC raw upload budget ignored')
        assert net.recognize_bgr(small, 32)[:2] == expected
    report = dict(passed=True, device=a.device, gpu_text_preprocess=True, rows=rows,
        library_sha256=hashlib.sha256(a.after.read_bytes()).hexdigest(),
        baseline_sha256=hashlib.sha256(a.before.read_bytes()).hexdigest(),
        raw_upload_budget_recovery='passed', limitations='Functional gate, not VRAM measurement or leak proof.')
    a.report.parent.mkdir(parents=True, exist_ok=True)
    a.report.write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
    print('PASS: GPU REC-only exact results / length contract / LRU / concurrency / budget recovery')


if __name__ == '__main__':
    main()
