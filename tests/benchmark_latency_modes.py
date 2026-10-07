"""Single-handle OCR latency/soak evidence; not a universal performance/leak claim.

Run one library/model per process, serially, with profiling disabled. Predecode
images, distinguish first use, warm calls, changing shapes and idle calls, and
retain resource samples. Item hashes test determinism, not ground-truth accuracy.
"""
import argparse
import ctypes as C
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import sys
import time

import numpy as np
from PIL import Image
import psutil

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'examples/python'))
from lwvk import OCR, DeviceInfo, check, load
from benchmark_projects.memory import MemoryMonitor, ram


def digest(items):
    return hashlib.sha256(json.dumps(items, sort_keys=True, ensure_ascii=False,
                                    allow_nan=False).encode('utf-8')).hexdigest()


def system_memory_snapshot():
    """Read-only host pressure, not a causal diagnosis or an OCR error gate."""
    try:
        value = psutil.virtual_memory()
        return dict(system_total_bytes=value.total, system_available_bytes=value.available,
                    system_memory_percent=value.percent, system_memory_error=None)
    except Exception as error:
        # Missing observations must stay unknown, never be mistaken for zero pressure.
        return dict(system_total_bytes=None, system_available_bytes=None,
                    system_memory_percent=None, system_memory_error=str(error))


def summarize(rows):
    if not rows:
        return {'calls': 0}
    keys = ('wall_ms', *rows[0]['timing'].keys())
    def values(key):
        return [r['wall_ms'] if key == 'wall_ms' else r['timing'][key] for r in rows]
    return dict(calls=len(rows), latency_ms={k: dict(
        median=float(np.median(values(k))), p95=float(np.percentile(values(k), 95)),
        minimum=min(values(k)), maximum=max(values(k))) for k in keys})


def schedule(count, image_count):
    # Alternate forward/reverse cycles; never repeat only one convenient shape.
    for i in range(count):
        cycle, offset = divmod(i, image_count)
        yield offset if cycle % 2 == 0 else image_count - 1 - offset


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--library', type=Path, required=True)
    p.add_argument('--model', choices=('tiny', 'small', 'medium'), required=True)
    p.add_argument('--device', type=int, default=1)
    p.add_argument('--iterations', type=int, default=40)
    p.add_argument('--stress-iterations', type=int, default=1000)
    p.add_argument('--idle-iterations', type=int, default=5)
    p.add_argument('--idle-seconds', type=float, default=2)
    p.add_argument('--corpus', type=Path, help='optional public img-*.jpg mixed-shape corpus')
    p.add_argument('--report', type=Path, required=True)
    a = p.parse_args()
    if not 1 <= a.iterations <= 5000 or not 0 <= a.stress_iterations <= 5000:
        p.error('iterations must be 1..5000 and stress-iterations 0..5000')
    if not 0 <= a.idle_iterations <= 30 or not math.isfinite(a.idle_seconds) or not 0 <= a.idle_seconds <= 30:
        p.error('idle-iterations/seconds must be 0..30')
    if any(os.environ.get(k) == '1' for k in ('LWVK_GPU_PROFILE', 'LWVK_HOST_PROFILE', 'LWVK_EXPERIMENTAL_COOP')):
        p.error('disable profiling and mixed-precision experiments for this FP32 latency run')
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, 'reconfigure'):
            stream.reconfigure(encoding='utf-8', errors='backslashreplace')
    images, metadata = [], []
    def add(image, label):
        value = np.ascontiguousarray(np.asarray(image.convert('RGB'))[:, :, ::-1])
        images.append(value)
        metadata.append(dict(id=label, width=value.shape[1], height=value.shape[0],
                             bgr_sha256=hashlib.sha256(value.tobytes()).hexdigest()))
    with Image.open(ROOT / 'test-images/sample.jpg') as image:
        add(image, 'sample-500')
        for size in ((250, 250), (1000, 1000), (900, 450), (450, 900)):
            with image.resize(size, Image.Resampling.BILINEAR) as scaled:
                add(scaled, 'sample-%dx%d' % size)
    if a.corpus:
        paths = sorted(a.corpus.glob('img-*.jpg'))
        if not paths:
            p.error('corpus contains no img-*.jpg images')
        for path in paths:
            with Image.open(path) as image:
                add(image, path.name)
    lib = load(a.library)
    info = DeviceInfo(struct_size=C.sizeof(DeviceInfo))
    check(lib, lib.lwvk_device_get(a.device, C.byref(info)))
    model_root = ROOT / 'models/onnx' / ('ppocrv6-' + a.model)
    report = dict(status='running', library_sha256=hashlib.sha256(a.library.read_bytes()).hexdigest(),
        version=lib.lwvk_version().decode(), model=a.model, device_index=a.device,
        device=info.name.decode('utf-8'), os=platform.platform(), images=metadata,
        model_sha256={t: hashlib.sha256((model_root / (t + '.onnx')).read_bytes()).hexdigest() for t in ('det', 'cls', 'rec')},
        phases={}, checkpoints=[], samples=[], idle_seconds=a.idle_seconds,
        method='one engine for all phases; predecoded BGR -> native OCR -> JSON copy/Python parsing; no file decode/UI/HTTP; default FP32, DET960, CLS enabled; no power-policy change',
        limitations='Deterministic item hashes are not GT accuracy. RAM includes Python/images/results/driver. WDDM counters are process attribution, not board VRAM; 250ms samples can miss peaks. Bounded growth/steady RSS is not leak proof. Independent before/after processes must be run serially; power/clocks are uncontrolled.')
    process = psutil.Process()
    memory = MemoryMonitor()
    expected = {}
    def checkpoint(phase, iteration):
        row = dict(phase=phase, iteration=iteration, **ram(process), threads=process.num_threads())
        row.update(system_memory_snapshot())
        if hasattr(process, 'num_handles'):
            row['handles'] = process.num_handles()
        report['checkpoints'].append(row)
        print(json.dumps(row), flush=True)
    def call(engine, phase, index, iteration):
        start = time.perf_counter()
        value = engine.run(images[index])
        wall = (time.perf_counter() - start) * 1000
        current = digest(value['items'])
        if index in expected and current != expected[index]:
            raise AssertionError(f'item fields changed at {phase}/{iteration}/image-{index}')
        expected[index] = current
        if not all(math.isfinite(v) and v >= 0 for v in value['timing'].values()):
            raise AssertionError('invalid timing')
        report['samples'].append(dict(phase=phase, image_index=index, iteration=iteration,
                                     wall_ms=wall, timing=value['timing']))
    memory.start()
    try:
        start = time.perf_counter()
        with OCR(lib, model_root, a.device) as engine:
            report['initialization_ms'] = (time.perf_counter() - start) * 1000
            call(engine, 'cold', 0, 0)
            for index in range(1, len(images)):
                call(engine, 'first_shape', index, index)
            for i, index in enumerate(schedule(2 * len(images), len(images))):
                call(engine, 'warmup', index, i)
            checkpoint('after-warmup', 0)
            for i in range(a.iterations):
                call(engine, 'steady', 0, i)
            for i, index in enumerate(schedule(a.iterations, 5)):
                call(engine, 'size_switch', index, i)
            for i, index in enumerate(schedule(a.stress_iterations, len(images))):
                call(engine, 'soak', index, i)
                if (i + 1) % 100 == 0:
                    checkpoint('soak', i + 1)
            for i in range(a.idle_iterations):
                time.sleep(a.idle_seconds)
                call(engine, 'idle', 0, i)
            checkpoint('before-close', a.stress_iterations)
        checkpoint('after-close', a.stress_iterations)
        report['status'] = 'passed'
        report['exact_repeated_item_hashes'] = True
    except BaseException as error:
        report.update(status='failed_or_interrupted', error=str(error))
        raise
    finally:
        report['memory'] = memory.finish()
        report['item_hashes'] = {str(k): v for k, v in expected.items()}
        warm = next((c for c in report['checkpoints'] if c['phase'] == 'after-warmup'), None)
        end = next((c for c in report['checkpoints'] if c['phase'] == 'before-close'), None)
        if warm and end:
            report['after_warmup_growth'] = {k: end[k] - warm[k] for k in
                ('rss_bytes', 'private_bytes', 'threads', 'handles')
                if warm.get(k) is not None and end.get(k) is not None}
        for phase in ('cold', 'first_shape', 'warmup', 'steady', 'size_switch', 'soak', 'idle'):
            report['phases'][phase] = summarize([r for r in report['samples'] if r['phase'] == phase])
        a.report.parent.mkdir(parents=True, exist_ok=True)
        a.report.write_text(json.dumps(report, indent=2, ensure_ascii=False, allow_nan=False) + '\n', encoding='utf-8')
    print(json.dumps(dict(status=report['status'], model=a.model,
        wall_ms={k: v.get('latency_ms', {}).get('wall_ms') for k, v in report['phases'].items()},
        after_warmup_growth=report.get('after_warmup_growth')), ensure_ascii=False))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
