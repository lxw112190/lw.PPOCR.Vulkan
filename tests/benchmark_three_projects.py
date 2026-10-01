"""Windows local benchmark: real C / legacy DML / Vulkan, metadata-backed images.

Each backend/model lives in an isolated process. Only one workload runs at a
time. Generated corpus text may be included in reports; do not use private data
without reviewing retention. This is a dataset benchmark, not a leak proof.
"""
import argparse
import ctypes as C
import hashlib
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
from PIL import Image
from benchmark_projects.adapters import CProject,DmlProject,VulkanProject
from benchmark_projects.memory import MemoryMonitor,ram

ROOT=Path(__file__).resolve().parents[1]


def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()


def write(path,value):
    path.parent.mkdir(parents=True,exist_ok=True)
    temporary=path.with_suffix(path.suffix+'.tmp')
    temporary.write_text(json.dumps(value,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    temporary.replace(path)


def dataset(args):
    sys.path.insert(0,str(ROOT.parent/'lw.PPOCR.C/tools'))
    import evaluate_ocr_dataset as evaluator
    manifest,records,warnings=evaluator.read_dataset(args.dataset)
    if len(records['images'])!=100:raise ValueError('expected the existing 100-image corpus')
    selected=records['images'][:args.limit] if args.limit else records['images']
    return evaluator,manifest,records,selected,warnings


def evaluate(evaluator,record,predictions):
    expected=record['lines'];width,height=record['width'],record['height']
    predicted=[]
    for item in predictions:
        text=evaluator.normalize_text(str(item['text']))
        coords=[float(item[f'{axis}{i}']) for i in range(1,5) for axis in ('x','y')]
        if not np.isfinite(coords).all():raise ValueError('nonfinite prediction coordinates')
        predicted.append(dict(text=text,box=evaluator.clamp_bbox([min(coords[::2]),min(coords[1::2]),
            max(coords[::2]),max(coords[1::2])],width,height)))
    gt=[evaluator.clamp_bbox(line['bbox'],width,height) for line in expected]
    matches=evaluator.match_lines(gt,predicted,.30)
    by_gt={g:(p,iou) for g,p,iou in matches};used={p for _,p,_ in matches}
    details=[];matched_edits=0;matched_chars=0;missing_chars=0;exact=0;all_chars=0
    for g,line in enumerate(expected):
        target=evaluator.normalize_text(line['text']);all_chars+=len(target)
        if g in by_gt:
            p,iou=by_gt[g];actual=predicted[p]['text'];distance=evaluator.edit_distance(target,actual)
            matched_edits+=distance;matched_chars+=len(target);same=target==actual;exact+=int(same)
        else:
            p=None;iou=0.;actual=None;distance=len(target);same=False;missing_chars+=len(target)
        details.append(dict(gt_index=g,expected=target,predicted=actual,matched=p is not None,
            prediction_index=p,iou=iou,exact=same,edit_distance=distance,
            category=line.get('category','unknown'),orientation_degrees=line.get('orientation_degrees',0)))
    extra_chars=sum(len(p['text']) for i,p in enumerate(predicted) if i not in used)
    return dict(gt_lines=len(expected),predicted_lines=len(predicted),matched_lines=len(matches),exact_lines=exact,
        gt_characters=all_chars,matched_gt_characters=matched_chars,matched_edits=matched_edits,
        missing_characters=missing_chars,extra_characters=extra_chars,
        end_to_end_edits=matched_edits+missing_chars+extra_chars,details=details)


def summarize(rows):
    keys=('gt_lines','predicted_lines','matched_lines','exact_lines','gt_characters','matched_gt_characters',
        'matched_edits','missing_characters','extra_characters','end_to_end_edits')
    totals={key:sum(r['accuracy'][key] for r in rows) for key in keys}
    g,p,m,ch,mc=(totals[k] for k in ('gt_lines','predicted_lines','matched_lines','gt_characters','matched_gt_characters'))
    cer=totals['end_to_end_edits']/ch if ch else 0.
    result=dict(**totals,exact_line_rate=totals['exact_lines']/g if g else 0.,
        detection_recall=m/g if g else 0.,detection_precision=m/p if p else 0.,
        matched_line_cer=totals['matched_edits']/mc if mc else None,
        end_to_end_cer=cer,end_to_end_character_accuracy=max(0.,1.-cer),
        normalization='NFC only; preserve whitespace/punctuation/case. Greedy one-to-one axis-aligned IoU >=0.30. Missing GT characters are deletions and unmatched prediction characters insertions.')
    result['orientation']={}
    for orientation in sorted({d['orientation_degrees'] for r in rows for d in r['accuracy']['details']}):
        values=[d for r in rows for d in r['accuracy']['details'] if d['orientation_degrees']==orientation]
        result['orientation'][str(orientation)]=dict(gt_lines=len(values),matched=sum(d['matched'] for d in values),
            exact=sum(d['exact'] for d in values),exact_line_rate=sum(d['exact'] for d in values)/len(values))
    return result


def decoded(root,record):
    with Image.open(root/record['file']) as im:
        return np.ascontiguousarray(np.asarray(im.convert('RGB'))[:,:,::-1])


def signature(items):
    return [(p['text'],*[round(float(p[f'{axis}{i}']),3) for i in range(1,5) for axis in ('x','y')]) for p in items]


def worker(a):
    evaluator,manifest,metadata,images,warnings=dataset(a)
    progress=a.output.parent/(a.output.stem+'-progress.json')
    update=lambda phase,index:write(progress,dict(backend=a.backend,model=a.variant,phase=phase,index=index,
        images=len(images),pid=os.getpid()))
    update('initializing',0);monitor=MemoryMonitor();baseline=ram(monitor.process);monitor.start();engine=None;rows=[]
    try:
        model_root=ROOT/'models/onnx'/('ppocrv6-'+a.variant);dictionary=model_root/'dictionary.txt'
        library={'c':a.c_library,'dml':a.dml_library,'vulkan':a.vk_library}[a.backend]
        started=time.perf_counter()
        if a.backend=='c':engine=CProject(library,a.c_models/a.variant,dictionary,a.c_workers)
        elif a.backend=='dml':engine=DmlProject(library,[a.dml_library.parent],model_root,dictionary,a.dxgi_device,a.dml_predictors,a.dml_batch)
        else:engine=VulkanProject(library,model_root,a.vk_device)
        initialization_ms=(time.perf_counter()-started)*1000;loaded=ram(monitor.process)
        # Warm every image once: cache turnover due to bounded plans remains part
        # of repeated dataset performance rather than being hidden by one shape.
        for i,record in enumerate(images):
            engine.run(decoded(manifest.parent,record))
            if (i+1)%10==0 or i+1==len(images):update('warmup',i+1)
        warmed=ram(monitor.process);failures=[];unstable=[];samples=[];round_memory=[]
        for repeat in range(a.repeats):
            for i,record in enumerate(images):
                bgr=decoded(manifest.parent,record);start=time.perf_counter();error=None
                try:items=engine.run(bgr)
                except Exception as e:items=[];error=str(e)
                elapsed=(time.perf_counter()-start)*1000
                if error:failures.append(dict(repeat=repeat,file=record['file'],error=error))
                else:samples.append(elapsed)
                if repeat==0:
                    rows.append(dict(file=record['file'],sha256=record['sha256'],size=[record['width'],record['height']],
                        predictions=items,accuracy=evaluate(evaluator,record,items),samples_ms=[elapsed],errors=[error] if error else []))
                else:
                    rows[i]['samples_ms'].append(elapsed)
                    if error:rows[i]['errors'].append(error)
                    if signature(items)!=signature(rows[i]['predictions']):unstable.append(dict(repeat=repeat,file=record['file']))
                if (i+1)%10==0 or i+1==len(images):update(f'measure-{repeat+1}',i+1)
            round_memory.append(dict(repeat=repeat+1,**ram(monitor.process)))
        ending=ram(monitor.process);memory=monitor.finish()
        report=dict(status='passed' if not failures else 'completed_with_errors',backend=a.backend,model=a.variant,
            library_sha256=sha(library),dataset_manifest_sha256=sha(manifest),images=len(images),
            source_models_sha256={name:sha(model_root/name) for name in ('det.onnx','cls.onnx','rec.onnx','dictionary.txt')},
            config=engine.config,method=dict(full_dataset_warmup_passes=1,repeats=a.repeats,
                timing='full OCR API call and conversion to Python result list; excludes file decode, ground-truth scoring, model loading and HTTP',
                workload='sequential image requests; backend internal parallelism follows stated config',iou=.30),
            initialization_ms=initialization_ms,ram_baseline=baseline,ram_loaded=loaded,ram_after_warmup=warmed,
            ram_after_measurement=ending,memory=memory,accuracy=summarize(rows),
            ram_after_each_repeat=round_memory,
            latency_ms=dict(mean=statistics.mean(samples),median=statistics.median(samples),p95=float(np.percentile(samples,95)),
                total=sum(samples),samples=len(samples)) if samples else None,
            failures=failures,unstable_content_or_boxes=unstable,warnings=warnings,results=rows)
        if a.backend=='c':report['runtime_models_sha256']={n:sha(a.c_models/a.variant/n) for n in ('det.lwm','cls.lwm','rec.lwm')}
        if a.backend=='dml':report['dependencies_sha256']={n:sha(library.parent/n) for n in ('onnxruntime.dll','DirectML.dll','opencv_world481.dll')}
        write(a.output,report);update('done',len(images));print(json.dumps(dict(backend=a.backend,model=a.variant,
            latency_ms=report['latency_ms'],exact_line_rate=report['accuracy']['exact_line_rate'])),flush=True)
    except BaseException:
        memory=monitor.finish() if monitor.thread.is_alive() else None
        write(a.output,dict(status='failed',backend=a.backend,model=a.variant,error=traceback.format_exc(),memory=memory))
        update('failed',len(rows));raise
    finally:
        if engine:engine.close()


def hardware(a):
    sys.path.insert(0,str(ROOT/'examples/python'));from lwvk import load,DeviceInfo,check
    vk=load(a.vk_library);info=DeviceInfo(struct_size=C.sizeof(DeviceInfo));check(vk,vk.lwvk_device_get(a.vk_device,C.byref(info)))
    helper=C.CDLL(str(a.adapter_library.resolve()))
    helper.lwvk_dml_adapter.argtypes=[C.c_uint32,C.c_void_p,C.c_uint32,C.POINTER(C.c_uint32),C.POINTER(C.c_uint32)]
    name=C.create_string_buffer(256);vendor,device=C.c_uint32(),C.c_uint32()
    if helper.lwvk_dml_adapter(a.dxgi_device,name,len(name),C.byref(vendor),C.byref(device)):
        raise ValueError('DXGI adapter index unavailable')
    if (vendor.value,device.value)!=(info.vendor_id,info.device_id) or 'RTX 4060' not in info.name.decode():
        raise ValueError('Vulkan/DXGI hardware mismatch')
    return dict(vulkan_index=a.vk_device,dxgi_index=a.dxgi_device,vulkan_name=info.name.decode(),dxgi_name=name.value.decode(),
        vendor_id=vendor.value,device_id=device.value,cpu=platform.processor(),logical_cpu_count=os.cpu_count(),
        os=platform.platform(),python=sys.version.split()[0],inventory_helper_only='DML reference DLL used only for DXGI enumeration in controller; actual DML inference uses original project DLL in its isolated worker')


def matrix(a):
    _,manifest,metadata,images,warnings=dataset(a);device=hardware(a);a.output.mkdir(parents=True,exist_ok=True)
    catalog=json.loads((ROOT/'models/onnx/catalog.json').read_text(encoding='utf-8'))
    for variant in ('tiny','small','medium'):
        models=ROOT/'models/onnx'/('ppocrv6-'+variant)
        for name,key in (('det.onnx','det_sha256'),('rec.onnx','rec_sha256'),('dictionary.txt','dictionary_sha256')):
            if sha(models/name)!=catalog[variant][key]:raise ValueError('model catalog hash mismatch: '+variant+'/'+name)
        if sha(models/'cls.onnx')!=catalog['cls_sha256']:raise ValueError('CLS catalog hash mismatch')
    reports=[]
    for index,variant in enumerate(('tiny','small','medium')):
        if a.smoke and variant!='tiny':continue
        backends=['c','dml','vulkan'];backends=backends[index:]+backends[:index]
        for backend in backends:
            output=a.output/(variant+'-'+backend+'.json');log=output.with_suffix('.log')
            command=[sys.executable,str(Path(__file__).resolve()),'--worker','--backend',backend,'--variant',variant,
                '--dataset',str(a.dataset.resolve()),'--output',str(output.resolve()),'--repeats',str(a.repeats),
                '--c-library',str(a.c_library.resolve()),'--c-models',str(a.c_models.resolve()),'--vk-library',str(a.vk_library.resolve()),
                '--dml-library',str(a.dml_library.resolve()),'--vk-device',str(a.vk_device),'--dxgi-device',str(a.dxgi_device),
                '--c-workers',str(a.c_workers),'--dml-predictors',str(a.dml_predictors),'--dml-batch',str(a.dml_batch)]
            if a.limit:command+=['--limit',str(a.limit)]
            env=os.environ.copy()
            for key in ('LWVK_GPU_PROFILE','LWVK_EXPERIMENTAL_COOP','LWVK_EXPERIMENTAL_COOP_DET_ONLY','LWVK_EXPERIMENTAL_TILE64'):
                env[key]='0'
            for key in ('VK_INSTANCE_LAYERS','LWVK_REFERENCE_GRAPH','LWVK_REFERENCE_KERNELS','LWVK_DISABLE_GELU_FUSION'):
                env.pop(key,None)
            for key in ('LWVK_DISABLE_TILED_POINTWISE','LWVK_DISABLE_TILED_GEMM','LWVK_DISABLE_VECTOR_DEPTHWISE'):
                env.pop(key,None)
            print('START '+variant+'/'+backend,flush=True);began=time.monotonic()
            with log.open('w',encoding='utf-8') as stream:
                proc=subprocess.Popen(command,env=env,stdout=stream,stderr=subprocess.STDOUT)
                while proc.poll() is None:
                    try:proc.wait(timeout=20)
                    except subprocess.TimeoutExpired:
                        progress=output.parent/(output.stem+'-progress.json')
                        if progress.exists():print(progress.read_text(encoding='utf-8').replace('\n',' '),flush=True)
                        if time.monotonic()-began>3600:proc.kill();proc.wait();raise TimeoutError('benchmark worker exceeded one hour')
            if not output.exists():raise RuntimeError(f'{variant}/{backend} exited {proc.returncode}; see {log}')
            report=json.loads(output.read_text(encoding='utf-8'));reports.append(report)
            write(a.output/'matrix.json',dict(status='running',hardware=device,reports=reports))
            if report['status']=='failed' or proc.returncode:raise RuntimeError(f'{variant}/{backend} failed; see {output}')
            print('DONE '+variant+'/'+backend+' mean_ms='+str(round(report['latency_ms']['mean'],2)),flush=True)
    status='passed' if all(r['status']=='passed' and not r['unstable_content_or_boxes'] for r in reports) else 'completed_with_errors'
    write(a.output/'matrix.json',dict(status=status,hardware=device,dataset_manifest_sha256=sha(manifest),
        images=len(images),gt_lines=sum(len(r['lines']) for r in images),repeats=a.repeats,warnings=warnings,
        harness_sha256={str(p.relative_to(ROOT)):sha(p) for p in (Path(__file__),
            ROOT/'tests/benchmark_projects/adapters.py',ROOT/'tests/benchmark_projects/memory.py')},
        reports=reports,caveat='Actual project pipelines, not identical mathematical inputs. C/Vulkan REC cap 960; DML batched widths 320..1280. RAM includes a common Python harness. Synthetic corpus accuracy does not establish real-world accuracy.'))


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--dataset',type=Path,default=ROOT.parent/'lw.PPOCR.C/build-local-data/lw-generated-ocr')
    p.add_argument('--output',type=Path,required=True);p.add_argument('--repeats',type=int,default=3)
    p.add_argument('--c-library',type=Path,default=ROOT.parent/'lw.PPOCR.C/build-vulkan-comparison/Release/lw_ppocr_c.dll')
    p.add_argument('--c-models',type=Path,default=ROOT/'build/comparison-c-models')
    p.add_argument('--dml-library',type=Path,default=ROOT/'build/comparison-dml/lw.OnnxRuntime.PPOCRSharp_dml.dll')
    p.add_argument('--vk-library',type=Path,default=ROOT/'build/gelu-optimized/Release/lw.PPOCR.Vulkan.dll')
    p.add_argument('--adapter-library',type=Path,default=ROOT/'build/gelu-optimized/Release/lwvk_dml_reference.dll')
    p.add_argument('--vk-device',type=int,default=1);p.add_argument('--dxgi-device',type=int,default=1)
    p.add_argument('--c-workers',type=int,default=8);p.add_argument('--dml-predictors',type=int,default=4)
    p.add_argument('--dml-batch',type=int,default=8);p.add_argument('--limit',type=int)
    p.add_argument('--smoke',action='store_true');p.add_argument('--worker',action='store_true',help=argparse.SUPPRESS)
    p.add_argument('--backend',choices=('c','dml','vulkan'),help=argparse.SUPPRESS)
    p.add_argument('--variant',choices=('tiny','small','medium'),help=argparse.SUPPRESS)
    a=p.parse_args()
    if os.name!='nt':p.error('this local three-project comparison requires Windows')
    if not 1<=a.repeats<=10 or (a.limit is not None and not 1<=a.limit<=100):p.error('invalid repeats/limit')
    for stream in (sys.stdout,sys.stderr):
        if hasattr(stream,'reconfigure'):stream.reconfigure(encoding='utf-8',errors='backslashreplace')
    if a.worker:
        if not a.backend or not a.variant:p.error('worker requires backend and variant')
        worker(a)
    else:matrix(a)
    return 0


if __name__=='__main__':raise SystemExit(main())
