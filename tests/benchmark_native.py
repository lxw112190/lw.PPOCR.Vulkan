"""Warm native C ABI benchmark, excluding HTTP/base64/image-file decoding."""
import argparse,hashlib,json,os,statistics,sys,time
from pathlib import Path
import numpy as np
from PIL import Image
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'examples/python'))
from lwvk import load,OCR
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--library',type=Path,required=True);p.add_argument('--models',type=Path,required=True)
p.add_argument('--device',type=int,default=1);p.add_argument('--iterations',type=int,default=10)
p.add_argument('--report',type=Path,required=True)
a=p.parse_args();assert 1<=a.iterations<=5000
if os.environ.get('LWVK_GPU_PROFILE')=='1':
    p.error('LWVK_GPU_PROFILE=1 perturbs execution; disable it for performance comparisons')
image=Path(__file__).resolve().parents[1]/'test-images/sample.jpg'
bgr=np.ascontiguousarray(np.asarray(Image.open(image).convert('RGB'))[:,:,::-1])
lib=load(a.library);samples=[]
with OCR(lib,a.models,a.device) as engine:
    for _ in range(3): expected=engine.run(bgr)
    for _ in range(a.iterations):
        start=time.perf_counter();result=engine.run(bgr);wall=(time.perf_counter()-start)*1000
        assert [x['text'] for x in result['items']]==[x['text'] for x in expected['items']]
        samples.append(dict(wall_ms=wall,**result['timing']))
report=dict(version=lib.lwvk_version().decode(),device=a.device,model_root=str(a.models),
    image_sha256=hashlib.sha256(image.read_bytes()).hexdigest(),library_sha256=hashlib.sha256(a.library.read_bytes()).hexdigest(),
    items=len(expected['items']),texts=[x['text'] for x in expected['items']],samples=samples,
    median_ms={k:statistics.median(x[k] for x in samples) for k in samples[0]},
    mean_ms={k:statistics.mean(x[k] for x in samples) for k in samples[0]},
    method={'warmup':3,'measured_iterations':a.iterations,'image_size':list(bgr.shape[:2]),
            'input':'predecoded contiguous BGR','classifier':True,'det_limit_side_len':960})
a.report.parent.mkdir(parents=True,exist_ok=True);a.report.write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
print(json.dumps(report['median_ms']))
