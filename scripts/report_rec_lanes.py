"""Compact, auditable 1/2/4-lane report; preserve controls and negative results."""
import argparse
import hashlib
import json
from pathlib import Path
import statistics
import numpy as np


def digest(value):
    return hashlib.sha256(json.dumps(value,sort_keys=True,ensure_ascii=False).encode()).hexdigest()


def summarize(matrices):
    references={};runs=[];all_rows=[]
    for matrix in matrices:
        if not matrix['passed'] or len(matrix['comparisons'])!=12:
            raise ValueError('need complete three-model four-mode matrices')
        rows=[]
        for source in matrix['comparisons']:
            if source['status']!='passed' or not source['exact_items_equal_to_baseline'] or source['failures'] or source['unstable_content_or_boxes']:
                raise ValueError('failed or non-exact matrix')
            predictions=[dict(file=row['file'],predictions=row['predictions']) for row in source['results']]
            if source['model'] in references and predictions!=references[source['model']]:
                raise ValueError('predictions changed between phases/modes')
            references[source['model']]=predictions
            keep=('model','mode','library_sha256','source_models_sha256','dataset_manifest_sha256','config',
                'initialization_ms','latency_ms','timing_ms','rec_lanes_requested','accuracy',
                'ram_after_each_repeat','exact_items_equal_to_baseline','different_image_files')
            row={key:source[key] for key in keep}
            row['memory']={key:value for key,value in source['memory'].items() if key!='samples'}
            row['prediction_sha256']=digest(predictions)
            row['per_image']=[dict(file=item['file'],samples_ms=item['samples_ms'],
                prediction_sha256=digest(item['predictions'])) for item in source['results']]
            rows.append(row)
        runs.append(dict(repeats=matrix['repeats'],reverse_order=matrix['reverse_order'],comparisons=rows))
        all_rows.extend(rows)
    aggregates=[]
    for model in ('tiny','small','medium'):
        for mode in ('baseline','lanes1','lanes2','lanes4'):
            rows=[row for row in all_rows if row['model']==model and row['mode']==mode]
            if len(rows)!=len(matrices):raise ValueError('missing or duplicate mode')
            if len({row['library_sha256'] for row in rows})!=1:raise ValueError('do not pool different binaries')
            samples=[sample for row in rows for image in row['per_image'] for sample in image['samples_ms']]
            def peak(key):
                values=[row['memory'][key] for row in rows if row['memory'].get(key) is not None]
                return max(values) if values else None
            aggregates.append(dict(model=model,mode=mode,samples=len(samples),
                mean_ms=statistics.mean(samples),median_ms=statistics.median(samples),p95_ms=float(np.percentile(samples,95)),
                os_lifetime_peak_working_set_bytes=peak('os_lifetime_peak_working_set_bytes'),
                private_peak_bytes=peak('private_peak_bytes'),gpu_dedicated_peak_bytes=peak('gpu_dedicated_peak_bytes'),
                gpu_shared_peak_bytes=peak('gpu_shared_peak_bytes')))
    return dict(passed=True,hardware=matrices[0]['hardware'],runs=runs,aggregates=aggregates,
        reference_predictions=references,exact_predictions_equal_across_all_modes_and_runs=True,
        measured_calls=sum(row['samples'] for row in aggregates),
        total_calls=sum(matrix['images']*(matrix['repeats']+1)*12 for matrix in matrices),
        decision='Do not enable by default. Preserve the qualified serial implementation; multi-queue code is a compile-time OFF-by-default research fork.',
        limitations='Single laptop, no power/clock locks. 250ms sampled per-PID WDDM memory; not physical board VRAM or leak proof. Latency excludes decode/model loading/UI/HTTP. No all-driver or multi-request throughput claim.')


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--runs',nargs='+',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args()
    result=summarize([json.loads(path.read_text(encoding='utf-8')) for path in a.runs])
    result['source_matrix_sha256']={str(path):hashlib.sha256(path.read_bytes()).hexdigest() for path in a.runs}
    a.output.parent.mkdir(parents=True,exist_ok=True)
    a.output.write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    for row in result['aggregates']:
        print(json.dumps(row),flush=True)


if __name__=='__main__':main()
