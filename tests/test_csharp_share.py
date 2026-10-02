"""Run the share package's real C# binaries with a development-free child PATH.

Checks all three models, cropped REC, app-local loader identity and optional
per-file manifest. This is deployment smoke, not a performance/leak test.
"""
import argparse
import ctypes as C
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--package',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--devices',type=int,nargs='+',default=[1])
    p.add_argument('--require-manifest',action='store_true')
    p.add_argument('--gpu-det-preprocess',action='store_true')
    p.add_argument('--gpu-text-preprocess',action='store_true')
    p.add_argument('--gpu-crop-preprocess',action='store_true')
    p.add_argument('--cpu-preprocess',action='store_true')
    a=p.parse_args()
    if a.cpu_preprocess and (a.gpu_det_preprocess or a.gpu_text_preprocess or a.gpu_crop_preprocess):
        p.error('CPU preprocessing cannot be combined with forced GPU preprocessing')
    if a.gpu_crop_preprocess and not (a.gpu_det_preprocess and a.gpu_text_preprocess):
        p.error('GPU crop requires both GPU DET and GPU text preprocessing')
    root=a.package.resolve()
    info=json.loads((root/'PACKAGE-INFO.json').read_text(encoding='utf-8'))
    assert sha(root/'lw.PPOCR.Vulkan.dll')==info['native_library_sha256']
    assert sha(root/'vulkan-1.dll')==info['bundled_loader_sha256']
    assert {p.name for p in root.glob('*.dll')}=={'vulkan-1.dll','lw.PPOCR.Vulkan.dll'}
    manifest=root/'FILES.sha256'
    count=0
    if manifest.exists():
        seen=set()
        for line in manifest.read_text(encoding='utf-8').splitlines():
            digest,name=line.split('  ',1)
            path=(root/name).resolve()
            assert path.is_relative_to(root) and name not in seen
            assert sha(path)==digest,name
            seen.add(name);count+=1
        assert seen=={p.relative_to(root).as_posix() for p in root.rglob('*') if p.is_file() and p!=manifest}
    elif a.require_manifest:raise AssertionError('missing per-file checksum manifest')
    # Prove the DLL under test resolves the bundled loader, not SDK PATH.
    with os.add_dll_directory(str(root)):
        native=C.CDLL(str(root/'lw.PPOCR.Vulkan.dll'))
        native.lwvk_version.restype=C.c_char_p
        assert native.lwvk_version().decode()==info['version']
        kernel=C.WinDLL('kernel32',use_last_error=True)
        kernel.GetModuleHandleW.argtypes=[C.c_wchar_p];kernel.GetModuleHandleW.restype=C.c_void_p
        kernel.GetModuleFileNameW.argtypes=[C.c_void_p,C.c_wchar_p,C.c_uint32]
        kernel.GetModuleFileNameW.restype=C.c_uint32
        filename=C.create_unicode_buffer(32768)
        assert kernel.GetModuleFileNameW(kernel.GetModuleHandleW('vulkan-1.dll'),filename,len(filename))
        assert Path(filename.value).resolve()==(root/'vulkan-1.dll').resolve(),filename.value
    env=os.environ.copy()
    for key in list(env):
        if key.upper().startswith(('VK_','LWVK_')) or key.upper() in ('VULKAN_SDK','VK_SDK_PATH'):
            env.pop(key,None)
    system=env.get('SystemRoot',env.get('SYSTEMROOT',r'C:\Windows'))
    for key in list(env):
        if key.upper()=='PATH':env.pop(key)
    env['PATH']=system+'\\System32;'+system+';'+system+'\\System32\\Wbem'
    if a.gpu_det_preprocess:
        env['LWVK_GPU_DET_PREPROCESS']='1'
    if a.gpu_text_preprocess:
        env['LWVK_GPU_TEXT_PREPROCESS']='1'
    if a.gpu_crop_preprocess:
        env['LWVK_GPU_CROP_PREPROCESS']='1'
    if a.cpu_preprocess:
        for key in ('LWVK_GPU_DET_PREPROCESS','LWVK_GPU_TEXT_PREPROCESS','LWVK_GPU_CROP_PREPROCESS'):
            env[key]='0'
    rows=[]
    with tempfile.TemporaryDirectory(prefix='lwvk-csharp-share-') as working:
        probe=subprocess.run([str(root/'lw-ppocr-vulkan-probe.exe')],cwd=working,env=env,
            capture_output=True,encoding='utf-8',errors='replace',timeout=30)
        assert probe.returncode==0,probe.stdout+probe.stderr
        assert '天天代码码天天' in probe.stdout and '\ufffd' not in probe.stdout
        for device in a.devices:
            for variant in ('tiny','small','medium'):
                models=root/'models/onnx'/('ppocrv6-'+variant)
                args=[str(root/'lw.PPOCR.Vulkan.CSharpDemo.exe'),'--ocr',str(models),
                    str(root/'test-images/sample.jpg'),str(device)]
                proc=subprocess.run(args,cwd=working,env=env,capture_output=True,
                    encoding='utf-8-sig',errors='replace',timeout=180)
                assert proc.returncode==0,proc.stdout+proc.stderr
                result=json.loads(proc.stdout[proc.stdout.index('{'):])
                assert len(result['items'])==16 and result['items'][0]['text']=='纯臻营养护发素'
                assert all('box' not in x for x in result['items'])
                args[1:] = ['--recognize',str(models/'rec.onnx'),str(root/'test-images/sample.jpg'),
                    str(device),'20','28','292','46']
                rec=subprocess.run(args,cwd=working,env=env,capture_output=True,
                    encoding='utf-8-sig',errors='replace',timeout=180)
                assert rec.returncode==0 and 'Text: 纯臻营养护发素' in rec.stdout,rec.stdout+rec.stderr
                rows.append(dict(device=device,model=variant,full_items=16,full_title_and_roi='passed'))
    report=dict(passed=True,package=root.name,version=info['version'],
        library_sha256=info['native_library_sha256'],loader_sha256=info['bundled_loader_sha256'],
        loader_origin='package directory verified via GetModuleFileNameW',
        development_path_removed=True,working_directory='outside package',
        checked_manifest_files=count,models_and_devices=rows,
        preprocessing_requested={key:env.get(key,'auto (unset)') for key in
            ('LWVK_GPU_DET_PREPROCESS','LWVK_GPU_TEXT_PREPROCESS','LWVK_GPU_CROP_PREPROCESS')},
        limitations='Installed local GPU drivers and .NET Framework are still present; not a clean OS VM, universal compatibility or a leak proof.')
    a.output.parent.mkdir(parents=True,exist_ok=True)
    a.output.write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print('PASS: bundled loader / real C# full OCR and ROI / three models / file checksums')
    return 0


if __name__=='__main__':raise SystemExit(main())
