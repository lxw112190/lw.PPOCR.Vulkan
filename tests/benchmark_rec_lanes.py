"""Isolated-process, serial GPU 1/2/4-lane experiment against the qualified DLL.

Shares dataset/scoring/resource sampling with the TensorRT benchmark, but runs
Vulkan only. Never run timing with validation/profiling layers enabled.
"""
import argparse
import json
import os
from pathlib import Path
import statistics
import subprocess
import sys
import time
import numpy as np
import benchmark_vulkan_trt as base
from benchmark_three_projects import dataset,write,ROOT


class TimedVulkanProject(base.VulkanProject):
    current=None

    def __init__(self,*args):
        super().__init__(*args)
        self.timings=[]
        TimedVulkanProject.current=self

    def run(self,bgr):
        result=self.engine.run(bgr)
        self.timings.append(result['timing'])
        return result['items']


def worker(a):
    base.VulkanProject=TimedVulkanProject
    a.exact_results=True
    base.worker(a)
    report=json.loads(a.output.read_text(encoding='utf-8'))
    measured=TimedVulkanProject.current.timings[report['images']:]
    report['rec_lanes_requested']=a.lanes
    report['timing_ms']={key:dict(mean=statistics.mean(row[key] for row in measured),
        median=statistics.median(row[key] for row in measured),
        p95=float(np.percentile([row[key] for row in measured],95))) for key in measured[0]}
    report['exact_items_repeat_comparison']=True
    report['timing_samples']=measured
    write(a.output,report)


def matrix(a):
    _,manifest,_,images,_=dataset(a)
    a.output.mkdir(parents=True,exist_ok=True)
    modes=[('baseline',1,a.before),('lanes1',1,a.vk_library),('lanes2',2,a.vk_library),('lanes4',4,a.vk_library)]
    reports=[]
    for index,variant in enumerate(a.models):
        order=modes[index%len(modes):]+modes[:index%len(modes)]
        if a.reverse:order=list(reversed(order))
        group=[]
        for name,lanes,library in order:
            output=a.output/f'{variant}-{name}.json'
            if output.exists():raise FileExistsError('use a fresh output directory')
            env={key:value for key,value in os.environ.items() if not key.upper().startswith(('LWVK_','VK_'))}
            env['LWVK_REC_LANES']=str(lanes)
            command=[sys.executable,str(Path(__file__).resolve()),'--worker','--variant',variant,
                '--lanes',str(lanes),'--dataset',str(a.dataset.resolve()),'--output',str(output.resolve()),
                '--vk-library',str(library.resolve()),'--vk-device',str(a.vk_device),'--repeats',str(a.repeats)]
            if a.limit:command+=['--limit',str(a.limit)]
            print('START '+variant+'/'+name,flush=True)
            with output.with_suffix('.log').open('w',encoding='utf-8') as stream:
                proc=subprocess.Popen(command,env=env,stdout=stream,stderr=subprocess.STDOUT)
                started=time.monotonic()
                while proc.poll() is None:
                    try:proc.wait(timeout=20)
                    except subprocess.TimeoutExpired:
                        if time.monotonic()-started>1200:
                            proc.kill();proc.wait();raise TimeoutError(command)
                        progress=output.with_name(output.stem+'-progress.json')
                        print(progress.read_text(encoding='utf-8').strip() if progress.exists() else 'loading',flush=True)
                if proc.returncode:raise RuntimeError(output.with_suffix('.log').read_text(encoding='utf-8'))
            report=json.loads(output.read_text(encoding='utf-8'));report['mode']=name
            if report['status']!='passed':raise AssertionError(f'{variant}/{name}: failures or unstable exact outputs')
            if report['memory']['gpu_counter_error']:raise AssertionError(report['memory']['gpu_counter_error'])
            group.append(report)
            print(json.dumps(dict(model=variant,mode=name,mean_ms=report['latency_ms']['mean'],
                p95_ms=report['latency_ms']['p95'],rec_ms=report['timing_ms']['rec_ms']['mean'])),flush=True)
        reference=next(row for row in group if row['mode']=='baseline')
        for report in group:
            differences=[row['file'] for row,wanted in zip(report['results'],reference['results'])
                if row['predictions']!=wanted['predictions']]
            report['exact_items_equal_to_baseline']=not differences
            report['different_image_files']=differences
            if differences:raise AssertionError(f'{variant}/{report["mode"]}: exact items changed: {differences}')
        reports.extend(group)
        write(a.output/'matrix.json',dict(passed=len(reports)==4*len(a.models),hardware=base.hardware(a),dataset_manifest=str(manifest),
            images=len(images),repeats=a.repeats,reverse_order=a.reverse,comparisons=reports,
            limitations='Single RTX 4060 laptop, serial fresh processes; per-PID memory sampled at 250ms. Not a leak proof or universal performance claim. Default remains one lane.'))
    print('PASS: exact item equality for all models/modes and repeats',flush=True)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--dataset',type=Path,default=ROOT.parent/'lw.PPOCR.C/build-local-data/lw-generated-ocr/metadata.json')
    p.add_argument('--before',type=Path,default=ROOT/'build/gpu-default/Release/lw.PPOCR.Vulkan.dll')
    p.add_argument('--vk-library',type=Path,default=ROOT/'build/rec-lanes-optin/Release/lw.PPOCR.Vulkan.dll')
    p.add_argument('--vk-device',type=int,default=1)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--repeats',type=int,default=5)
    p.add_argument('--models',nargs='+',choices=('tiny','small','medium'),default=['tiny','small','medium'])
    p.add_argument('--limit',type=int)
    p.add_argument('--reverse',action='store_true')
    p.add_argument('--worker',action='store_true',help=argparse.SUPPRESS)
    p.add_argument('--variant',choices=('tiny','small','medium'),help=argparse.SUPPRESS)
    p.add_argument('--lanes',type=int,choices=(1,2,4),default=1,help=argparse.SUPPRESS)
    a=p.parse_args();a.backend='vulkan'
    if os.name!='nt':p.error('this resource benchmark requires Windows WDDM')
    if not 1<=a.repeats<=10 or (a.limit is not None and not 1<=a.limit<=100):p.error('invalid repeat/limit')
    for stream in (sys.stdout,sys.stderr):
        if hasattr(stream,'reconfigure'):stream.reconfigure(encoding='utf-8',errors='backslashreplace')
    if a.worker:worker(a)
    else:matrix(a)


if __name__=='__main__':main()
