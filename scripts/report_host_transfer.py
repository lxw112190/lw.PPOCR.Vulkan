"""Audit an FP32 update against the preserved 100-image benchmark.

This writes evidence artifacts, not model/source locks. It refuses changed
inputs/configuration/output fields rather than silently relaxing accuracy gates.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import shutil

ROOT = Path(__file__).resolve().parents[1]
MODELS = ('tiny', 'small', 'medium')


def read(path):
    return json.loads(path.read_text(encoding='utf-8'))


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def audit_pair(old, new):
    for report in (old, new):
        if (report['status'] != 'passed' or report['failures'] or
                report['unstable_content_or_boxes']):
            raise ValueError('failed/unstable benchmark cannot qualify an optimization')
        if (report['images'] != 100 or len(report['results']) != 100 or
                report['method']['repeats'] != 3 or report['latency_ms']['samples'] != 300):
            raise ValueError('expected 100 images and three measured passes')
    for key in ('backend', 'model', 'dataset_manifest_sha256', 'source_models_sha256', 'config', 'method'):
        if old[key] != new[key]:
            raise ValueError('comparison conditions changed: ' + key)
    for a, b in zip(old['results'], new['results']):
        for key in ('file', 'sha256', 'size', 'predictions', 'accuracy'):
            if a[key] != b[key]:
                raise ValueError('changed ' + key + ': ' + a['file'])
    if old['accuracy'] != new['accuracy']:
        raise ValueError('aggregate accuracy changed')
    return dict(model=new['model'], images=100, exact_prediction_objects=100,
                aggregate_accuracy_equal=True, old_latency_ms=old['latency_ms'],
                new_latency_ms=new['latency_ms'],
                mean_latency_reduction_percent=100*(1-new['latency_ms']['mean']/old['latency_ms']['mean']))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--baseline', type=Path, default=ROOT/'docs/reports/three-project-100/matrix.json')
    p.add_argument('--results', type=Path, required=True)
    p.add_argument('--library', type=Path, required=True)
    p.add_argument('--dml', type=Path, required=True)
    p.add_argument('--workspace', type=Path, required=True)
    p.add_argument('--reference', type=Path, action='append', default=[])
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    baseline = read(a.baseline)
    digest = sha(a.library)
    qualified = []
    artifacts = {}
    for model in MODELS:
        path = a.results/(model+'-vulkan.json')
        new = read(path)
        old = next(r for r in baseline['reports'] if r['model'] == model and r['backend'] == 'vulkan')
        if new['library_sha256'] != digest:
            raise ValueError('benchmark DLL does not match candidate')
        qualified.append(audit_pair(old, new))
        compact = copy.deepcopy(new)
        compact['memory'].pop('samples', None)
        artifacts[model+'-vulkan.json'] = compact
    dml, workspace = read(a.dml), read(a.workspace)
    if (dml['status'] != 'passed' or not workspace['passed'] or
            any(r['library_sha256'] != digest for r in (dml, workspace))):
        raise ValueError('DML/workspace qualification failed or uses a different DLL')
    if (dml['method']['iterations'] < 30 or not dml['method']['dml_shape_cache'] or
            dml['method']['experimental_coop_requested'] or
            dml['method']['experimental_tile64_requested']):
        raise ValueError('expected qualified default FP32 / cached DML comparison')
    for row in dml['comparisons']:
        if not row['accuracy']['text_equal'] or not row['accuracy']['cls_equal']:
            raise ValueError('DML text/classifier comparison failed')
    sources = ['CMakeLists.txt', 'src/memory_policy.hpp', 'src/vulkan_context.hpp',
               'src/vulkan_context.cpp', 'src/det_preprocess.hpp', 'src/det_preprocess.cpp',
               'src/ocr_host.cpp', 'src/graph.cpp', 'tests/memory_policy_unit.cpp',
               'tests/det_preprocess_unit.cpp', 'tests/benchmark_three_projects.py',
               'tests/benchmark_projects/adapters.py', 'tests/benchmark_projects/memory.py',
               'tests/benchmark_dml_ocr.py', 'tests/test_workspace.py',
               'tests/test_ocr_reference.py', 'scripts/report_host_transfer.py']
    provenance = dict(library_sha256=digest, baseline_sha256=sha(a.baseline),
        source_sha256={name:sha(ROOT/name) for name in sources},
        hardware=baseline['hardware'], dataset_manifest_sha256=baseline['dataset_manifest_sha256'],
        audit=qualified,
        evidence_input_sha256={str(path):sha(path) for path in
            [a.dml, a.workspace, *a.reference, *(a.results/(m+'-vulkan.json') for m in MODELS)]},
        caveat='Before/after corpus runs were sequential, not paired in the same time window. '
            'DML helper is a separate matched-host experiment, not the legacy project DLL. '
            'ORT reference reports are invocation evidence; they do not independently embed a DLL hash. '
            'RAM/WDDM counters are not a leak proof or physical-board VRAM measurement.')
    a.output.mkdir(parents=True, exist_ok=True)
    for name, value in {**artifacts, 'qualification.json':provenance}.items():
        (a.output/name).write_text(json.dumps(value,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    for path in (a.dml, a.workspace, *a.reference):
        target = a.output/path.name
        if path.resolve() != target.resolve():
            shutil.copyfile(path, target)
    print(json.dumps(dict(library_sha256=digest, audit=qualified),ensure_ascii=False,indent=2))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
