"""GPU-crop option contract, shared-device CLS-off and independent OCR handles.

Run serially on a physical GPU. This is not device-loss injection or a soak.
"""
import argparse
import hashlib
import json
import os
import subprocess
from pathlib import Path
import sys
import numpy as np
from PIL import Image
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'examples/python'))
from lwvk import OCR,load

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--library',type=Path,required=True)
    p.add_argument('--device',type=int,default=1)
    p.add_argument('--report',type=Path,required=True)
    p.add_argument('--child',choices=['reject','run'])
    p.add_argument('--classifier',type=int,choices=[0,1],default=1)
    a=p.parse_args();root=ROOT/'models/onnx/ppocrv6-tiny'
    if a.child:
        lib=load(a.library)
        if a.child=='reject':
            try:
                engine=OCR(lib,root,a.device)
            except RuntimeError as error:
                assert 'requires both GPU preprocessing flags' in str(error)
            else:
                engine.close();raise AssertionError('incomplete crop prerequisites accepted')
            print(json.dumps(dict(passed=True)));return
        with Image.open(ROOT/'test-images/sample.jpg') as image:
            pixels=np.ascontiguousarray(np.asarray(image.convert('RGB'))[:,:,::-1])
        with OCR(lib,root,a.device,enable_classifier=a.classifier) as first, OCR(lib,root,a.device,enable_classifier=a.classifier) as second:
            expected=first.run(pixels)['items']
            for engine in (first,second,first,second):
                assert engine.run(pixels)['items']==expected
                assert not engine.run(np.full((65,83,3),255,dtype=np.uint8))['items']
            assert first.run(pixels)['items']==expected
        if os.environ.get('LWVK_GPU_CROP_PREPROCESS') in (None,'auto','1') and all(
            os.environ.get(key)!='0' for key in ('LWVK_GPU_DET_PREPROCESS','LWVK_GPU_TEXT_PREPROCESS')):
            with OCR(lib,root,a.device,det_limit_side=32,max_workspace_bytes=1024*1024) as engine:
                small=np.full((16,16,3),255,dtype=np.uint8)
                assert not engine.run(small)['items']
                rejected=False
                # Uploaded source fits the cap, but can leave too little room for
                # network activations. The next small image must release its watermark.
                for side in range(256,416,8):
                    try:engine.run(np.full((side,side,3),255,dtype=np.uint8))
                    except RuntimeError as error:
                        assert 'max_workspace_bytes exceeded' in str(error)
                        assert 'shared image/crops' not in str(error),str(error)
                        rejected=True;break
                assert rejected,'network-pressure rejection was not exercised'
                assert not engine.run(small)['items']
        print(json.dumps(dict(passed=True,digest=hashlib.sha256(json.dumps(expected,sort_keys=True).encode()).hexdigest())))
        return
    # Set flags before each child's DLL load. Windows /MT CRT getenv snapshots
    # must not be mistaken for dynamically changing process environment options.
    def child(det,text,crop,mode,classifier=1):
        env=os.environ.copy()
        env.pop('LWVK_GPU_PROFILE',None)
        for key,value in zip(('LWVK_GPU_DET_PREPROCESS','LWVK_GPU_TEXT_PREPROCESS','LWVK_GPU_CROP_PREPROCESS'),(det,text,crop)):
            if value is None:env.pop(key,None)
            else:env[key]=str(value)
        result=subprocess.run([sys.executable,__file__,'--library',str(a.library.resolve()),'--device',str(a.device),
            '--report',str(a.report),'--child',mode,'--classifier',str(classifier)],env=env,capture_output=True,encoding='utf-8',timeout=180)
        assert result.returncode==0,result.stdout+result.stderr
        lines=result.stdout.splitlines()
        # Validation layers may write startup messages to stdout. Preserve all
        # diagnostics for the caller's VUID/SYNC-HAZARD scan, parse only our JSON.
        diagnostics='\n'.join(lines[:-1])+result.stderr
        if diagnostics:print(diagnostics,file=sys.stderr,flush=True)
        return json.loads(lines[-1])
    for det,text in ((0,0),(1,0),(0,1)):child(det,text,1,'reject')
    for classifier in (1,0):
        expected=child(1,1,1,'run',classifier)['digest']
        for options in ((1,1,0),(None,None,None),('auto','auto','auto'),(0,0,0),(0,None,None),(None,0,None)):
            assert child(*options,'run',classifier)['digest']==expected,(options,classifier)
    report=dict(passed=True,device=a.device,library_sha256=hashlib.sha256(a.library.read_bytes()).hexdigest(),
        missing_prerequisites='3 rejected in fresh processes',classifier_on_off='exact items',independent_handles='alternating image/blank/recovery',
        retained_source_budget_recovery='network-pressure rejection followed by smaller image passed',
        default_auto_and_explicit='unset / auto / forced GPU / CPU preprocessing / partial disable match for CLS on/off',
        limitations='Serial local GPU contract, not concurrent multi-handle stress, memory/leak proof or device-loss injection')
    a.report.parent.mkdir(parents=True,exist_ok=True)
    a.report.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print('PASS: GPU crop prerequisites / CLS disabled / independent handles')

if __name__=='__main__':main()
