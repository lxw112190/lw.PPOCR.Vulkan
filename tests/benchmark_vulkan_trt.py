"""Serial, isolated-process three-model Vulkan vs original TensorRT project benchmark."""
import argparse
import ctypes as C
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess
import sys
import time
import traceback
import numpy as np
from benchmark_three_projects import dataset,decoded,evaluate,summarize,signature,sha,write,ROOT
from benchmark_projects.adapters import VulkanProject
from benchmark_projects.trt_adapter import TrtProject
from benchmark_projects.gpu_activity import ResourceMonitor,NvidiaBoard
from benchmark_projects.memory import ram


def hardware(a):
    sys.path.insert(0,str(ROOT/'examples/python'))
    from lwvk import load,DeviceInfo,check
    lib=load(a.vk_library);info=DeviceInfo(struct_size=C.sizeof(DeviceInfo))
    check(lib,lib.lwvk_device_get(a.vk_device,C.byref(info)))
    cuda=C.WinDLL('nvcuda.dll');cuda.cuDeviceGetName.argtypes=[C.c_void_p,C.c_int,C.c_int]
    if cuda.cuInit(0):raise RuntimeError('CUDA initialization failed')
    count=C.c_int()
    if cuda.cuDeviceGetCount(C.byref(count)) or count.value!=1:raise RuntimeError('benchmark expects exactly one CUDA device')
    name=C.create_string_buffer(256)
    if cuda.cuDeviceGetName(name,len(name),0):raise RuntimeError('CUDA device name unavailable')
    board=NvidiaBoard()
    try:
        if info.name.decode()!=name.value.decode() or board.info['name']!=info.name.decode():
            raise RuntimeError('Vulkan/CUDA/NVML device mismatch')
        import winreg
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,r'HARDWARE\DESCRIPTION\System\CentralProcessor\0') as key:
            cpu=winreg.QueryValueEx(key,'ProcessorNameString')[0].strip()
        return dict(vulkan_index=a.vk_device,cuda_index=0,vulkan_name=info.name.decode(),
            vendor_id=info.vendor_id,device_id=info.device_id,nvidia=board.info,cpu=cpu,
            os=platform.platform(),logical_cpu_count=os.cpu_count(),python=sys.version.split()[0])
    finally:board.close()


def worker(a):
    evaluator,manifest,metadata,images,warnings=dataset(a)
    progress=a.output.with_name(a.output.stem+'-progress.json')
    def update(phase,index):write(progress,dict(backend=a.backend,model=a.variant,phase=phase,index=index,pid=os.getpid()))
    monitor=ResourceMonitor();baseline=ram(monitor.process);monitor.start();engine=None;rows=[]
    try:
        update('initializing',0);models=ROOT/'models/onnx'/('ppocrv6-'+a.variant)
        library=a.vk_library if a.backend=='vulkan' else a.trt_runtime/'lw.TensorRT.PPOCRSharp.dll'
        started=time.perf_counter()
        if a.backend=='vulkan':engine=VulkanProject(library,models,a.vk_device)
        else:engine=TrtProject(library,a.trt_runtime,a.trt_engines,models,a.variant,a.predictors,a.batch)
        init=(time.perf_counter()-started)*1000;loaded=ram(monitor.process)
        monitor.phase='warmup'
        for i,record in enumerate(images):
            engine.run(decoded(manifest.parent,record))
            if (i+1)%10==0:update('warmup',i+1)
        warmed=ram(monitor.process);samples=[];failures=[];unstable=[];round_memory=[]
        monitor.phase='measure'
        for repeat in range(a.repeats):
            # Reverse every other pass to expose changing-shape cache turnover.
            order=range(len(images)) if repeat%2==0 else range(len(images)-1,-1,-1)
            for step,i in enumerate(order):
                record=images[i];bgr=decoded(manifest.parent,record);error=None;start=time.perf_counter()
                try:items=engine.run(bgr)
                except Exception as e:items=[];error=str(e)
                elapsed=(time.perf_counter()-start)*1000
                if error:failures.append(dict(repeat=repeat,file=record['file'],error=error))
                else:samples.append(elapsed)
                if repeat==0:
                    rows.append(dict(file=record['file'],size=[record['width'],record['height']],sha256=record['sha256'],
                        predictions=items,samples_ms=[elapsed],errors=[error] if error else []))
                else:
                    rows[i]['samples_ms'].append(elapsed)
                    if error:rows[i]['errors'].append(error)
                    changed=(items!=rows[i]['predictions']) if getattr(a,'exact_results',False) else (signature(items)!=signature(rows[i]['predictions']))
                    if changed:unstable.append(dict(repeat=repeat,file=record['file']))
                if (step+1)%10==0:update('measure-'+str(repeat+1),step+1)
            round_memory.append(ram(monitor.process))
        monitor.phase='after';ending=ram(monitor.process);resources=monitor.finish()
        # Score only after measurement, so GT matching doesn't dilute GPU activity.
        for record,row in zip(images,rows):row['accuracy']=evaluate(evaluator,record,row['predictions'])
        report=dict(status='passed' if not failures and not unstable else 'completed_with_errors',
            backend=a.backend,model=a.variant,images=len(images),library_sha256=sha(library),
            source_models_sha256={name:sha(models/name) for name in ('det.onnx','cls.onnx','rec.onnx','dictionary.txt')},
            dataset_manifest_sha256=sha(manifest),config=engine.config,initialization_ms=init,
            ram_baseline=baseline,ram_loaded=loaded,ram_after_warmup=warmed,ram_after_measurement=ending,
            ram_after_each_repeat=round_memory,memory=resources,
            latency_ms=dict(mean=statistics.mean(samples),median=statistics.median(samples),
                p95=float(np.percentile(samples,95)),samples=len(samples),total=sum(samples)),
            accuracy=summarize(rows),failures=failures,unstable_content_or_boxes=unstable,warnings=warnings,results=rows,
            method=dict(warmup_passes=1,repeats=a.repeats,alternate_direction=True,
                latency='full native OCR + result JSON conversion; excludes decode/model initialization/GT scoring/UI/HTTP',
                utilization='sustained measured stream including between-call image decode/JSON work; scoring deferred until after sampling'))
        if a.backend=='trt':
            report['engine_sha256']={key:sha(path) for key,path in engine.assets.items()}
            report['engine_paths']={key:str(path.resolve()) for key,path in engine.assets.items()}
            report['dependency_sha256']={name:sha(a.trt_runtime/name) for name in ('nvinfer_10.dll','nvinfer_plugin_10.dll','cudart64_12.dll','opencv_world481.dll')}
        else:report['preprocessing']='all three flags unset; native capability-aware default on selected RTX 4060'
        write(a.output,report);update('done',len(images))
        print(json.dumps(dict(backend=a.backend,model=a.variant,latency_ms=report['latency_ms'],gpu=resources['gpu_activity'])),flush=True)
    except BaseException:
        resources=monitor.finish() if monitor.thread.is_alive() else None
        write(a.output,dict(status='failed',backend=a.backend,model=a.variant,error=traceback.format_exc(),memory=resources))
        update('failed',len(rows));raise
    finally:
        if engine:engine.close()


def matrix(a):
    _,manifest,metadata,images,warnings=dataset(a);device=hardware(a)
    for variant in ('tiny','small','medium'):
        models=ROOT/'models/onnx'/('ppocrv6-'+variant)
        for task in ('det','rec'):
            source=a.trt_runtime/'inference'/f'PP-OCRv6_{variant}_{task}.onnx'
            if sha(source)!=sha(models/(task+'.onnx')):raise ValueError('TRT source ONNX mismatch '+variant+'/'+task)
            if sha(a.trt_engines.parent/'inference'/source.name)!=sha(source):raise ValueError('engine-side source mismatch')
        if sha(a.trt_runtime/'inference'/f'PP-OCRv6_{variant}_rec_dict.txt')!=sha(models/'dictionary.txt'):
            raise ValueError('TRT source dictionary mismatch')
        if sha(a.trt_runtime/'inference/PP-OCRv5_mobile_cls_onnx.onnx')!=sha(models/'cls.onnx'):
            raise ValueError('CLS source mismatch')
    a.output.mkdir(parents=True,exist_ok=True);reports=[]
    for index,variant in enumerate(('tiny','small','medium')):
        for backend in (('trt','vulkan') if index%2==0 else ('vulkan','trt')):
            output=a.output/f'{variant}-{backend}.json';log=output.with_suffix('.log')
            if output.exists():raise FileExistsError('use a fresh report directory')
            command=[sys.executable,str(Path(__file__).resolve()),'--worker','--backend',backend,'--variant',variant,
                '--dataset',str(a.dataset.resolve()),'--output',str(output.resolve()),'--repeats',str(a.repeats),
                '--vk-library',str(a.vk_library.resolve()),'--vk-device',str(a.vk_device),
                '--trt-runtime',str(a.trt_runtime.resolve()),'--trt-engines',str(a.trt_engines.resolve()),
                '--predictors',str(a.predictors),'--batch',str(a.batch)]
            if a.limit:command+=['--limit',str(a.limit)]
            env=os.environ.copy()
            for key in list(env):
                if key.upper().startswith(('LWVK_','VK_')):env.pop(key,None)
            print('START '+variant+'/'+backend,flush=True)
            started=time.monotonic()
            with log.open('w',encoding='utf-8') as stream:
                proc=subprocess.Popen(command,env=env,stdout=stream,stderr=subprocess.STDOUT)
                while proc.poll() is None:
                    try:proc.wait(timeout=20)
                    except subprocess.TimeoutExpired:
                        progress=output.with_name(output.stem+'-progress.json')
                        if progress.exists():print(progress.read_text(encoding='utf-8').replace('\n',' '),flush=True)
                        if time.monotonic()-started>1800:
                            proc.kill();proc.wait();raise TimeoutError('worker exceeded 30 minutes')
            if not output.exists():raise RuntimeError('missing report; see '+str(log))
            report=json.loads(output.read_text(encoding='utf-8'));reports.append(report)
            write(a.output/'matrix.json',dict(status='running',hardware=device,reports=reports))
            if proc.returncode or report['status']!='passed':raise RuntimeError('worker failed: '+str(output))
            print('DONE '+variant+'/'+backend+' mean_ms='+str(round(report['latency_ms']['mean'],2)),flush=True)
    write(a.output/'matrix.json',dict(status='passed',hardware=device,images=len(images),repeats=a.repeats,
        dataset_manifest_sha256=sha(manifest),reports=reports,
        harness_sha256={str(path.relative_to(ROOT)):sha(path) for path in (Path(__file__),
            ROOT/'tests/benchmark_projects/trt_adapter.py',ROOT/'tests/benchmark_projects/gpu_activity.py',
            ROOT/'tests/benchmark_projects/memory.py',ROOT/'tests/benchmark_projects/adapters.py',ROOT/'tests/benchmark_three_projects.py')},
        caveat='Actual pipelines: Vulkan FP32 vs existing FP16-enabled TRT plans; REC widths/parallelism differ. Available ONNX/dictionary hashes match, but existing plans have no signed source-build attestation. Synthetic corpus, single laptop, no clock/power locking; not universal performance or leak proof.'))


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--dataset',type=Path,default=ROOT.parent/'lw.PPOCR.C/build-local-data/lw-generated-ocr')
    p.add_argument('--output',type=Path,required=True);p.add_argument('--repeats',type=int,default=3)
    p.add_argument('--vk-library',type=Path,default=ROOT/'build/gpu-default/Release/lw.PPOCR.Vulkan.dll')
    p.add_argument('--vk-device',type=int,default=1)
    p.add_argument('--trt-runtime',type=Path,default=ROOT.parent/'PP-OCRv5_Test_trt/OCRV5Test/bin/x64/Release')
    p.add_argument('--trt-engines',type=Path,default=ROOT.parent/'TensorRT-10.16.1.11.Windows.amd64.cuda-12.9/TensorRT-10.16.1.11/bin/inference_trt')
    p.add_argument('--predictors',type=int,default=4);p.add_argument('--batch',type=int,default=4)
    p.add_argument('--limit',type=int);p.add_argument('--worker',action='store_true',help=argparse.SUPPRESS)
    p.add_argument('--backend',choices=('vulkan','trt'),help=argparse.SUPPRESS)
    p.add_argument('--variant',choices=('tiny','small','medium'),help=argparse.SUPPRESS)
    a=p.parse_args()
    if os.name!='nt':p.error('Windows WDDM benchmark only')
    if not 1<=a.repeats<=10 or (a.limit is not None and not 1<=a.limit<=100):p.error('invalid repeats/limit')
    if not 1<=a.predictors<=8 or not 1<=a.batch<=16:p.error('invalid predictors/batch')
    for stream in (sys.stdout,sys.stderr):
        if hasattr(stream,'reconfigure'):stream.reconfigure(encoding='utf-8',errors='backslashreplace')
    if a.worker:
        if not a.backend or not a.variant:p.error('worker needs backend/variant')
        worker(a)
    else:matrix(a)

if __name__=='__main__':main()
