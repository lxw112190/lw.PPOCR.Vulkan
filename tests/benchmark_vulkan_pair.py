"""Alternating native OCR before/after comparison with exact item equality."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import statistics
import sys
import time
import numpy as np
from PIL import Image
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'examples/python'))
from lwvk import load,OCR


def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--before',type=Path,required=True);p.add_argument('--after',type=Path,required=True)
    p.add_argument('--device',type=int,default=1);p.add_argument('--iterations',type=int,default=30)
    p.add_argument('--sizes',type=int,nargs='+',default=[500,1000])
    p.add_argument('--models',nargs='+',choices=['tiny','small','medium'],default=['tiny','small','medium'])
    p.add_argument('--report',type=Path,required=True)
    a=p.parse_args();assert 1<=a.iterations<=5000
    if any(os.environ.get(x)=='1' for x in ('LWVK_GPU_PROFILE','LWVK_HOST_PROFILE')):
        p.error('disable diagnostic profiling for latency comparisons')
    libs=[load(a.before),load(a.after)];rows=[]
    for model in a.models:
        for size in a.sizes:
            with Image.open(ROOT/'test-images/sample.jpg') as im:
                if size!=500:im=im.resize((size,size),Image.Resampling.BILINEAR)
                pixels=np.ascontiguousarray(np.asarray(im.convert('RGB'))[:,:,::-1])
            engines=[OCR(lib,ROOT/'models/onnx'/('ppocrv6-'+model),a.device) for lib in libs]
            try:
                expected=None;samples=[[],[]]
                for _ in range(3):
                    for engine in engines:
                        result=engine.run(pixels)
                        if expected is None:expected=result['items']
                        assert expected==result['items'],'before/after warm-up result changed'
                for iteration in range(a.iterations):
                    for which in ((0,1) if iteration%2==0 else (1,0)):
                        start=time.perf_counter();result=engines[which].run(pixels);wall=(time.perf_counter()-start)*1000
                        assert result['items']==expected,'before/after item fields changed'
                        samples[which].append(dict(wall_ms=wall,**result['timing']))
                medians=[{k:statistics.median(x[k] for x in s) for k in s[0]} for s in samples]
                row=dict(model=model,size=size,regions=len(expected),exact_items_equal=True,
                    item_digest=hashlib.sha256(json.dumps(expected,sort_keys=True).encode()).hexdigest(),
                    before_median_ms=medians[0],after_median_ms=medians[1],samples=samples,
                    wall_reduction_percent=100*(1-medians[1]['wall_ms']/medians[0]['wall_ms']))
                rows.append(row);print(json.dumps({k:v for k,v in row.items() if k not in ('samples','item_digest')},ensure_ascii=True),flush=True)
            finally:
                for engine in engines:engine.close()
    report=dict(passed=True,before_sha256=sha(a.before),after_sha256=sha(a.after),device=a.device,
        method=dict(warmup_calls=3,iterations=a.iterations,order='alternating and reversed on odd iterations',
            gpu_det_preprocess_environment=os.environ.get('LWVK_GPU_DET_PREPROCESS','0'),
            gpu_text_preprocess_environment=os.environ.get('LWVK_GPU_TEXT_PREPROCESS','0'),
            scope='predecoded BGR -> native OCR -> JSON copy and Python parsing; no GUI/file decoding; one active GPU workload',
            det_limit_side=960,classifier=True,precision='default FP32'),comparisons=rows)
    a.report.parent.mkdir(parents=True,exist_ok=True)
    a.report.write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')


if __name__=='__main__':main()
