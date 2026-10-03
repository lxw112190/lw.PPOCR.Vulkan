"""Compare small/large switching in isolated processes, with exact item hashes."""
import argparse
import ctypes as C
import hashlib
import json
import os
from pathlib import Path
import statistics
import subprocess
import sys
import time

import numpy as np
from PIL import Image
import psutil

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'examples/python'))
from lwvk import DeviceInfo, OCR, check, load


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def measure(args, library):
    lib = load(library)
    info = DeviceInfo(); info.struct_size = C.sizeof(info)
    check(lib, lib.lwvk_device_get(args.device, C.byref(info)))
    images = {}
    for name, path in [('small', args.small_image), ('large', args.large_image)]:
        with Image.open(path) as image:
            images[name] = np.ascontiguousarray(np.asarray(image.convert('RGB'))[:, :, ::-1])
    process = psutil.Process()
    samples = []; expected = {}
    def run(engine, phase, name):
        started = time.perf_counter(); result = engine.run(images[name])
        wall = (time.perf_counter() - started) * 1000
        digest = hashlib.sha256(json.dumps(result['items'], sort_keys=True, ensure_ascii=False).encode()).hexdigest()
        if name in expected and expected[name] != digest:
            raise AssertionError('Output changed across sizes: ' + name)
        expected[name] = digest
        timing = result['timing']
        samples.append(dict(phase=phase, image=name, wall_ms=wall, timing=timing,
                            other_ms=timing['total_ms'] - sum(timing[k] for k in ('det_ms', 'cls_ms', 'rec_ms')),
                            count=len(result['items']), items_sha256=digest, rss_bytes=process.memory_info().rss))
    with OCR(lib, args.model_root, args.device) as engine:
        for name in ['small'] * 3 + ['large'] * 3 + ['small'] * 3:
            run(engine, 'warmup', name)
        for _ in range(6): run(engine, 'small_baseline', 'small')
        for _ in range(args.rounds):
            run(engine, 'alternating', 'large')
            run(engine, 'alternating', 'small')
        for _ in range(6): run(engine, 'small_tail', 'small')
        memory = process.memory_info()
    summaries = {}
    for phase, name in [('small_baseline', 'small'), ('alternating', 'small'),
                        ('alternating', 'large'), ('small_tail', 'small')]:
        rows = [r for r in samples if r['phase'] == phase and r['image'] == name]
        summaries[phase + '_' + name] = dict(
            count=len(rows), median_wall_ms=statistics.median(r['wall_ms'] for r in rows),
            median_total_ms=statistics.median(r['timing']['total_ms'] for r in rows),
            median_other_ms=statistics.median(r['other_ms'] for r in rows),
            first_rss_bytes=rows[0]['rss_bytes'], last_rss_bytes=rows[-1]['rss_bytes'])
    return dict(library_sha256=sha(library), version=lib.lwvk_version().decode(),
                device=args.device, device_name=info.name.decode('utf-8', 'replace'),
                model_root_name=args.model_root.name,
                images={k:dict(width=v.shape[1], height=v.shape[0]) for k,v in images.items()},
                image_sha256=dict(small=sha(args.small_image), large=sha(args.large_image)),
                summaries=summaries, samples=samples, items_sha256=expected,
                peak_rss_bytes=getattr(memory, 'peak_wset', max(r['rss_bytes'] for r in samples)),
                end_rss_bytes=memory.rss, handles=getattr(process, 'num_handles', lambda: None)())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--before', type=Path)
    parser.add_argument('--after', type=Path, required=True)
    parser.add_argument('--small-image', type=Path, required=True)
    parser.add_argument('--large-image', type=Path, required=True)
    parser.add_argument('--model-root', type=Path, required=True)
    parser.add_argument('--device', type=int, default=1)
    parser.add_argument('--rounds', type=int, default=20)
    parser.add_argument('--report', type=Path)
    parser.add_argument('--worker', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    if not 1 <= args.rounds <= 500:
        parser.error('rounds must be 1..500')
    if any(os.environ.get(k) == '1' for k in ('LWVK_HOST_PROFILE', 'LWVK_GPU_PROFILE')):
        parser.error('disable diagnostic profiling for timings')
    if args.worker:
        print(json.dumps(measure(args, args.after), ensure_ascii=True))
        return
    if args.before is None or args.report is None:
        parser.error('--before and --report are required for comparison')
    measurements = {}
    for name, path in [('before', args.before), ('after', args.after)]:
        command = [sys.executable, str(Path(__file__).resolve()), '--worker', '--after', str(path.resolve()),
                   '--small-image', str(args.small_image.resolve()), '--large-image', str(args.large_image.resolve()),
                   '--model-root', str(args.model_root.resolve()), '--device', str(args.device),
                   '--rounds', str(args.rounds)]
        result = subprocess.run(command, capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=1800)
        if result.returncode:
            raise RuntimeError(result.stderr + result.stdout)
        measurements[name] = json.loads(result.stdout)
        print(name, json.dumps(measurements[name]['summaries']), flush=True)
    before, after = measurements['before'], measurements['after']
    assert before['items_sha256'] == after['items_sha256'], 'Before/after OCR output differs'
    assert before['image_sha256'] == after['image_sha256'], 'Inputs changed during comparison'
    report = dict(passed=True, exact_items_equal=True, measurements=measurements,
                  method='Separate processes; predecoded BGR; FP32 DET960 CLS on; warmup then small baseline, '
                         'alternating large/small, small tail. No initialization/GUI time in samples.',
                  limitations='One machine/input pair; RSS and handle samples, not VRAM or leak proof. '
                              'Cold plans remain slower; finite cache can still evict larger working sets.')
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print('PASS: exact items preserved; report=' + str(args.report))


if __name__ == '__main__':
    main()
