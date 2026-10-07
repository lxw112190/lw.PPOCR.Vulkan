"""Paired short-REC kernels: exact probabilities plus uninstrumented latency.

Deterministic normalized tensors isolate changing REC widths. Not GT accuracy
or full OCR throughput. Library allocations coexist, so repeat reversed order.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import statistics
import sys
import time
import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'examples/python'))
from lwvk import Network, load


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--before', type=Path, required=True)
    p.add_argument('--after', type=Path, required=True)
    p.add_argument('--device', type=int, default=1)
    p.add_argument('--iterations', type=int, default=30)
    p.add_argument('--widths', type=int, nargs='+', default=[32, 48, 64, 96, 128, 192, 320, 960])
    p.add_argument('--reverse-initialization', action='store_true')
    p.add_argument('--models', nargs='+', choices=['tiny', 'small', 'medium'], default=['tiny', 'small', 'medium'])
    p.add_argument('--report', type=Path, required=True)
    a = p.parse_args()
    if not 1 <= a.iterations <= 5000 or any(w < 32 or w > 960 or w % 8 for w in a.widths):
        p.error('iterations must be 1..5000; widths are multiples of 8 in 32..960')
    if any(os.environ.get(k) == '1' for k in ('LWVK_GPU_PROFILE', 'LWVK_HOST_PROFILE')):
        p.error('disable diagnostic instrumentation for latency comparisons')
    order = (1, 0) if a.reverse_initialization else (0, 1)
    paths = [a.before, a.after]
    libs = [None, None]
    for which in order:
        libs[which] = load(paths[which])
    rows = []
    for model in a.models:
        model_path = ROOT / 'models/onnx' / ('ppocrv6-' + model) / 'rec.onnx'
        networks = [None, None]
        try:
            for which in order:
                networks[which] = Network(libs[which], model_path, a.device)
            for width in a.widths:
                tensor = np.random.default_rng(20261007 + width).uniform(-1, 1, (1, 3, 48, width)).astype(np.float32)
                probabilities = [n.run(tensor)[0] for n in networks]
                assert probabilities[0].tobytes() == probabilities[1].tobytes(), (model, width, 'probability bits differ')
                expected = None
                samples = [[], []]
                for i in range(a.iterations + 3):
                    for which in ((0, 1) if i % 2 == 0 else (1, 0)):
                        start = time.perf_counter()
                        text, score, stage = networks[which].recognize(tensor)
                        wall = (time.perf_counter() - start) * 1000
                        if expected is None:
                            expected = (text, score)
                        assert (text, score) == expected, (model, width, 'CTC text/score differ')
                        if i >= 3:
                            samples[which].append(dict(wall_ms=wall, stage_ms=stage))
                medians = [statistics.median(r['wall_ms'] for r in s) for s in samples]
                rows.append(dict(model=model, width=width, model_sha256=hashlib.sha256(model_path.read_bytes()).hexdigest(),
                    exact_probability_bits=True, exact_text_score=True,
                    output_sha256=hashlib.sha256(probabilities[0].tobytes()).hexdigest(), samples=samples,
                    median_ms=medians, p95_ms=[float(np.percentile([r['wall_ms'] for r in s], 95)) for s in samples],
                    reduction_percent=100 * (1 - medians[1] / medians[0])))
                print(json.dumps({k: rows[-1][k] for k in ('model', 'width', 'median_ms', 'p95_ms', 'reduction_percent')}), flush=True)
        finally:
            for network in networks:
                if network:
                    network.close()
    report = dict(passed=True, before_sha256=hashlib.sha256(a.before.read_bytes()).hexdigest(),
        after_sha256=hashlib.sha256(a.after.read_bytes()).hexdigest(), device=a.device, comparisons=rows,
        initialization_order='candidate-first' if a.reverse_initialization else 'baseline-first',
        method='normalized random FP32 tensor -> REC -> GPU greedy CTC -> Python text; three warmups per shape; paired alternating order, exact full probability bits compared outside timing; no OCR/image preprocessing/GT accuracy claim')
    a.report.parent.mkdir(parents=True, exist_ok=True)
    a.report.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
