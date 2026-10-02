"""Alternating old/new OCR over a changing image stream, without per-image warmup."""
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

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--before',type=Path,required=True);p.add_argument('--after',type=Path,required=True)
    p.add_argument('--corpus',type=Path,required=True);p.add_argument('--device',type=int,default=1)
    p.add_argument('--passes',type=int,default=2);p.add_argument('--report',type=Path,required=True)
    a=p.parse_args();assert 1<=a.passes<=10
    if any(os.environ.get(x)=='1' for x in ('LWVK_GPU_PROFILE','LWVK_HOST_PROFILE')):
        p.error('disable diagnostic profiling for timing')
    corpus=sorted(a.corpus.glob('img-*.jpg'));assert len(corpus)==100
    images=[]
    for path in corpus:
        with Image.open(path) as image:images.append(np.ascontiguousarray(np.asarray(image.convert('RGB'))[:,:,::-1]))
    libs=[load(a.before),load(a.after)];rows=[]
    for model in ('tiny','small','medium'):
        engines=[OCR(lib,ROOT/'models/onnx'/('ppocrv6-'+model),a.device) for lib in libs]
        try:
            passes=[];expected=[None]*100
            for phase in range(a.passes):
                samples=[[],[]]
                for index in (range(100) if phase%2==0 else range(99,-1,-1)):
                    for which in ((0,1) if (index+phase)%2==0 else (1,0)):
                        start=time.perf_counter();result=engines[which].run(images[index]);wall=(time.perf_counter()-start)*1000
                        if expected[index] is None:expected[index]=result['items']
                        assert result['items']==expected[index],(model,phase,index,'items differ')
                        samples[which].append(dict(image_index=index,wall_ms=wall,total_ms=result['timing']['total_ms']))
                means=[statistics.mean(x['wall_ms'] for x in path) for path in samples]
                record=dict(pass_index=phase,mean_wall_ms=means,reduction_percent=100*(1-means[1]/means[0]),samples=samples)
                passes.append(record);print(json.dumps(dict(model=model,pass_index=phase,mean_wall_ms=means,reduction_percent=record['reduction_percent'])),flush=True)
            rows.append(dict(model=model,exact_items_equal=True,passes=passes))
        finally:
            for engine in engines:engine.close()
    report=dict(passed=True,device=a.device,before_sha256=hashlib.sha256(a.before.read_bytes()).hexdigest(),
        after_sha256=hashlib.sha256(a.after.read_bytes()).hexdigest(),comparisons=rows,
        gpu_det_preprocess=os.environ.get('LWVK_GPU_DET_PREPROCESS','0'),gpu_text_preprocess=os.environ.get('LWVK_GPU_TEXT_PREPROCESS','0'),
        method='100 predecoded images; alternating old/new order; no per-image warmup; pass 0 includes first-use plans, pass 1 reverses image order; FP32, DET960, CLS enabled; excludes model initialization and GUI',
        limitations='One local changing stream, not universal latency or memory/leak qualification')
    a.report.parent.mkdir(parents=True,exist_ok=True)
    a.report.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')

if __name__=='__main__':main()
