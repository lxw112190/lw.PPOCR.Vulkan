"""Qualify exact FP32 host-pipeline outputs and preserve benchmark provenance."""
import argparse
import copy
import json
from pathlib import Path
import shutil
from report_host_transfer import audit_pair, read, sha

ROOT = Path(__file__).resolve().parents[1]
MODELS = ('tiny', 'small', 'medium')

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--library', type=Path, required=True)
    p.add_argument('--results', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    digest = sha(a.library)
    baseline = read(ROOT/'docs/reports/host-transfer/qualification.json')
    paired = [read(a.results/f) for f in ('final-pair.json', 'amd-pair.json')]
    for pair in paired:
        assert pair['passed'] and pair['after_sha256'] == digest
        assert pair['before_sha256'] == baseline['library_sha256']
        assert pair['method']['det_limit_side'] == 960 and pair['method']['precision'] == 'default FP32'
        assert all(r['exact_items_equal'] for r in pair['comparisons'])
    assert paired[0]['method']['iterations'] == 30 and len(paired[0]['comparisons']) == 6
    workspace = read(a.results/'workspace.json')
    assert workspace['passed'] and workspace['library_sha256'] == digest
    assert workspace['private_image'] and workspace['private_image']['repeated_results'] == 'consistent'
    log = (ROOT/'build/pipeline-opt/Testing/Temporary/LastTest.log').read_text(errors='replace')
    assert log.count('Test Passed.') == 12 and 'Test Failed.' not in log
    audits = []
    artifacts = ['final-pair.json', 'amd-pair.json', 'workspace.json', 'shared-layout-pair.json']
    a.output.mkdir(parents=True, exist_ok=True)
    for model in MODELS:
        path = a.results/'corpus'/(model+'-vulkan.json')
        new = read(path)
        assert new['library_sha256'] == digest
        audits.append(audit_pair(read(ROOT/'docs/reports/host-transfer'/(model+'-vulkan.json')),new))
        compact = copy.deepcopy(new)
        compact['memory'].pop('samples',None)
        (a.output/(model+'-vulkan.json')).write_text(json.dumps(compact,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
        ref_name = 'reference-'+model+'.json'
        ref = read(a.results/ref_name)
        assert ref['input_recovery'] == ref['result_lifetime'] == ref['classifier_disabled'] == 'passed'
        assert ref['concurrency'] == '4 workers / 4 calls passed'
        assert [(x['variant'],x['items']) for x in ref['comparisons']] == [('sample',16),('rotated-180',16),('blank',0)]
        artifacts.append(ref_name)
    sources = ['CMakeLists.txt','src/ocr_host.cpp','src/ocr_host.hpp','src/cls_preprocess.cpp',
        'src/cls_preprocess.hpp','src/rec_preprocess.cpp','src/crop_optimized.c','src/crop_optimized.hpp',
        'tests/resize_preprocess_unit.cpp','tests/crop_optimized_unit.cpp','tests/benchmark_vulkan_pair.py',
        'tests/test_workspace.py','tests/test_ocr_reference.py','scripts/prepare_shaders.py',
        'scripts/report_pipeline_optimization.py','third_party/lw-ppocr-c/ppocr/crop.c',
        'NOTICE','dependencies.lock.json','scripts/package_csharp_demo.py','tests/summarize_demo_revision.py']
    evidence = [a.results/f for f in artifacts]+[a.results/'corpus'/(m+'-vulkan.json') for m in MODELS]
    evidence.append(ROOT/'build/pipeline-opt/Testing/Temporary/LastTest.log')
    result = dict(passed=True,library_sha256=digest,baseline_library_sha256=baseline['library_sha256'],
        baseline_qualification_sha256=sha(ROOT/'docs/reports/host-transfer/qualification.json'),
        hardware=baseline['hardware'],dataset_manifest_sha256=baseline['dataset_manifest_sha256'],
        source_sha256={f:sha(ROOT/f) for f in sources},
        evidence_input_sha256={str(f.relative_to(ROOT) if f.is_absolute() else f):sha(f) for f in evidence},
        audit=audits,ctest='12/12 passed; resize: 90 bit-identical tensors; crop: 72 byte-identical cases',
        default_path='FP32; DET 960; classifier enabled; shaders/models/API unchanged; experiments and profiling off',
        discarded_experiment='Shared-memory row padding produced no stable benefit in six paired cases; reverted.',
        caveat='Paired sample runs alternate old/new in the same process; corpus runs are sequential time windows. '
            'AMD Small/Medium did not show a speedup. ORT reference reports are invocation evidence, not self-hashed DLL attestations. '
            'No new long-run leak proof or universal GPU performance claim. Private-image report stores no OCR text/image bytes.')
    (a.output/'qualification.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    for f in artifacts:shutil.copyfile(a.results/f,a.output/f)
    shutil.copyfile(ROOT/'build/pipeline-opt/Testing/Temporary/LastTest.log',a.output/'ctest.log')
    print(json.dumps(dict(library_sha256=digest,audit=audits),ensure_ascii=False,indent=2))

if __name__ == '__main__':main()
