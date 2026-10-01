"""Varied-size full OCR/REC HTTP stability run; RSS is evidence, not leak proof."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import statistics
import sys
import time
from PIL import Image
import psutil
from http_common import Service,json_request
for stream in (sys.stdout,sys.stderr):
    if hasattr(stream,'reconfigure'): stream.reconfigure(encoding='utf-8',errors='backslashreplace')
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--package',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--device',type=int,default=0);p.add_argument('--iterations',type=int,default=1000)
p.add_argument('--models',type=Path)
p.add_argument('--warmup-iterations',type=int,default=0,
               help='Additional varied-size OCR/REC warmup before the RSS baseline; recorded separately')
a=p.parse_args();assert 1<=a.iterations<=5000
assert 0<=a.warmup_iterations<=1000
variants=[]
with Image.open(a.package/'test-images/sample.jpg') as original:
    for size in ((250,250),(500,500),(1000,1000),(900,450),(450,900)):
        with original.resize(size) as image:
            buf=io.BytesIO();image.save(buf,format='PNG');variants.append((size,buf.getvalue()))
    with original.crop((20,28,312,74)) as image:
        buf=io.BytesIO();image.save(buf,format='PNG');crop=buf.getvalue()
overrides={'model_root':str(a.models.resolve())} if a.models else {}
with Service(a.package,a.output,a.device,**overrides) as s:
    process=psutil.Process(s.process.pid);baselines=[]
    for _,image in variants:
        result=json_request(s,'/api/ocr',image)['result'];baselines.append([i['text'] for i in result['items']])
    json_request(s,'/api/recognize',crop)
    rss_initial=process.memory_info().rss;warmup_samples=[]
    for i in range(a.warmup_iterations):
        index=i%len(variants)
        value=json_request(s,'/api/ocr',variants[index][1])
        assert [v['text'] for v in value['result']['items']]==baselines[index]
        if i%25==0:assert json_request(s,'/api/recognize',crop)['result']['text']=='纯臻营养护发素'
        if (i+1)%50==0 or i+1==a.warmup_iterations:
            sample={'warmup_iteration':i+1,'rss_bytes':process.memory_info().rss}
            warmup_samples.append(sample);print(json.dumps(sample),flush=True)
    rss_base=process.memory_info().rss; samples=[];latency=[];start=time.monotonic()
    for i in range(a.iterations):
        index=i%len(variants);size,image=variants[index];t=time.monotonic()
        value=json_request(s,'/api/ocr',image);assert [v['text'] for v in value['result']['items']]==baselines[index],(i,size,value)
        latency.append((time.monotonic()-t)*1000)
        if i%25==0:
            value=json_request(s,'/api/recognize',crop);assert value['result']['text']=='纯臻营养护发素'
        if (i+1)%50==0 or i+1==a.iterations:
            sample={'iteration':i+1,'rss_bytes':process.memory_info().rss,'threads':process.num_threads()}
            if hasattr(process,'num_handles'): sample['handles']=process.num_handles()
            samples.append(sample);print(json.dumps(sample),flush=True)
    elapsed=time.monotonic()-start
    json_request(s,'/health');rss_end=process.memory_info().rss
    rss_ok=rss_end-rss_base<96*1024*1024
    report={'ok':rss_ok,'request_results_consistent':True,'rss_signal_threshold_bytes':96*1024*1024,
        'version':json_request(s,'/api/info')['version'],'device':a.device,'full_ocr_requests':a.iterations,
        'model_root':s.config['model_root'],
        'recognize_requests':1+(a.iterations+24)//25,'sizes':[list(v[0]) for v in variants],
        'elapsed_seconds':elapsed,'rss_after_warmup':rss_base,'rss_end':rss_end,'rss_delta':rss_end-rss_base,
        'rss_after_first_size_cycle':rss_initial,'extra_warmup_ocr_requests':a.warmup_iterations,
        'extra_warmup_recognize_requests':(a.warmup_iterations+24)//25,'warmup_samples':warmup_samples,
        'samples':samples,'latency_ms':{'median':statistics.median(latency),'p95':sorted(latency)[int((len(latency)-1)*.95)]},
        'limits':'RSS/handles/threads only; no sanitizer or GPU VRAM leak-proof claim'}
    report['service_sha256']=hashlib.sha256(s.exe.read_bytes()).hexdigest()
    library=s.package/('lw.PPOCR.Vulkan.dll' if sys.platform=='win32' else 'liblw.PPOCR.Vulkan.so')
    report['library_sha256']=hashlib.sha256(library.read_bytes()).hexdigest()
(a.output/'stress.json').write_text(json.dumps(report,indent=2,ensure_ascii=False),encoding='utf-8')
print(json.dumps(report,ensure_ascii=False))
assert rss_ok,('RSS growth exceeded 96 MiB signal threshold',rss_base,rss_end)
