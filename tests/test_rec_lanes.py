"""Opt-in REC lanes: synchronized native probes and complete GPU-crop OCR.

Separate from timing; validation layers can dramatically alter latency.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
ROOT=Path(__file__).resolve().parents[1]


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build',type=Path,default=ROOT/'build/rec-lanes-optin/Release')
    p.add_argument('--device',type=int,default=1)
    p.add_argument('--models',nargs='+',choices=('tiny','small','medium'),default=['tiny','small','medium'])
    p.add_argument('--lanes',nargs='+',type=int,choices=(1,2,4),default=[1,2,4])
    p.add_argument('--report',type=Path,required=True)
    a=p.parse_args();a.report.parent.mkdir(parents=True,exist_ok=True)
    rows=[]
    env={key:value for key,value in os.environ.items() if not key.upper().startswith(('LWVK_','VK_'))}
    env.update(VK_INSTANCE_LAYERS='VK_LAYER_KHRONOS_validation',VK_LAYER_VALIDATE_SYNC='1',
        VK_LOADER_DEBUG='layer',LWVK_GPU_TEXT_PREPROCESS='1')
    for model in a.models:
        for lanes in a.lanes:
            env['LWVK_REC_LANES']=str(lanes)
            commands=[('rec-probe',[str(a.build/'lwvk_rec_batch_probe.exe'),
                str(ROOT/'models/onnx'/('ppocrv6-'+model)/'rec.onnx'),str(a.device)])]
            # The Windows resource monitor is RTX-specific; native probes above
            # can also exercise AMD. Do not call it a full AMD OCR qualification.
            if a.device==1:
                commands.append(('full-ocr',[sys.executable,str(ROOT/'tests/benchmark_rec_lanes.py'),'--worker',
                    '--variant',model,'--lanes',str(lanes),'--vk-device',str(a.device),'--vk-library',
                    str(a.build/'lw.PPOCR.Vulkan.dll'),'--limit','8','--repeats','2','--output',
                    str(a.report.parent/f'validation-{model}-{lanes}-ocr.json')]))
            for kind,command in commands:
                proc=subprocess.run(command,cwd=ROOT,env=env,capture_output=True,text=True,
                    encoding='utf-8',errors='replace',timeout=180)
                log=proc.stdout+proc.stderr
                path=a.report.parent/f'validation-{a.device}-{model}-{lanes}-{kind}.log'
                path.write_text(log,encoding='utf-8')
                if proc.returncode or 'Validation Error' in log or 'SYNC-HAZARD' in log or 'VUID-' in log:
                    raise AssertionError(f'{model}/{lanes}/{kind} failed; see {path}')
                if 'VK_LAYER_KHRONOS_validation' not in log:
                    raise AssertionError('validation-layer loading not confirmed: '+str(path))
                if kind=='rec-probe' and f'effective_lanes={lanes}' not in log:
                    raise AssertionError('requested lanes were not exercised; build with LWVK_EXPERIMENTAL_REC_LANES=ON')
                if kind=='full-ocr':
                    result=json.loads((a.report.parent/f'validation-{model}-{lanes}-ocr.json').read_text(encoding='utf-8'))
                    if result['status']!='passed':raise AssertionError('full OCR not stable')
                rows.append(dict(model=model,lanes=lanes,kind=kind,passed=True,
                    layer_loaded=True,synchronization_validation=True,log_sha256=hashlib.sha256(log.encode()).hexdigest()))
                print(f'PASS: device={a.device} {model} lanes={lanes} {kind}',flush=True)
    library=a.build/'lw.PPOCR.Vulkan.dll'
    a.report.write_text(json.dumps(dict(passed=True,device=a.device,checks=rows,
        library_sha256=hashlib.sha256(library.read_bytes()).hexdigest(),
        limitations='Local functional/synchronization checks, not timing, leak proof or all-driver qualification.'),indent=2)+'\n',encoding='utf-8')


if __name__=='__main__':main()
