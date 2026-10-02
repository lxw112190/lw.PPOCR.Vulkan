"""Prepare or archive the reviewed Windows-x64 C# sharing bundle.

Two phases: prepare -> test staged contents -> archive -> test exact extraction.
No driver copying, installation, system modification or automatic downloads.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[1]
RUNTIME_VERSION = '1.4.350.0'
RUNTIME_ZIP_SHA = '23ce69f32cef3e2799617e2b1776cd0c71030d23a91f8375821cc40d76b185b9'
RUNTIME_EXE_SHA = '2e26a920afe0cc7adb49c3133fb9dd36fc7a4a1e93277717800c48b32eefffdf'
SKIP = shutil.ignore_patterns('__pycache__','*.pyc','*.pyo','bin','obj','.vs','*.user','*.suo')


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream,'sha256').hexdigest() if hasattr(hashlib,'file_digest') else hashlib.sha256(stream.read()).hexdigest()


def write_json(path, value):
    path.parent.mkdir(parents=True,exist_ok=True)
    path.write_text(json.dumps(value,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')


def require_x64(path):
    raw=path.read_bytes()
    if raw[:2]!=b'MZ':raise ValueError('not a PE binary: '+str(path))
    offset=struct.unpack_from('<I',raw,0x3c)[0]
    if raw[offset:offset+4]!=b'PE\0\0' or struct.unpack_from('<H',raw,offset+4)[0]!=0x8664:
        raise ValueError('expected Windows x64 binary: '+str(path))


def verify_models(root):
    catalog=json.loads((root/'models/onnx/catalog.json').read_text(encoding='utf-8'))
    for variant in ('tiny','small','medium'):
        for file,key in (('det.onnx','det_sha256'),('rec.onnx','rec_sha256'),
                         ('cls.onnx','cls_sha256'),('dictionary.txt','dictionary_sha256')):
            expected=catalog[key] if key=='cls_sha256' else catalog[variant][key]
            if sha(root/'models/onnx'/('ppocrv6-'+variant)/file)!=expected:
                raise ValueError('model checksum mismatch: '+variant+'/'+file)


def prepare(a):
    if a.staging.exists():raise FileExistsError('use a fresh staging directory; do not overwrite a tested package')
    subprocess.run([sys.executable,str(ROOT/'scripts/verify_assets.py')],cwd=ROOT,check=True)
    if sha(a.runtime_components)!=RUNTIME_ZIP_SHA or sha(a.runtime_installer)!=RUNTIME_EXE_SHA:
        raise ValueError('official LunarG runtime archive/installer checksum mismatch')
    dll=a.build/'Release/lw.PPOCR.Vulkan.dll'
    qualification=json.loads(a.qualification.read_text(encoding='utf-8'))
    if sha(dll)!=qualification['library_sha256']:
        raise ValueError('candidate does not match the qualified FP32 DLL')
    a.staging.mkdir(parents=True)
    for source,name in [(dll,dll.name),(a.build/'Release/lw-ppocr-vulkan-probe.exe','lw-ppocr-vulkan-probe.exe'),
        (a.build/'examples/lw.PPOCR.Vulkan.WinFormsDemo.exe','lw.PPOCR.Vulkan.WinFormsDemo.exe'),
        (a.build/'examples/lw.PPOCR.Vulkan.CSharpDemo.exe','lw.PPOCR.Vulkan.CSharpDemo.exe'),
        (ROOT/'examples/winforms/App.config','lw.PPOCR.Vulkan.WinFormsDemo.exe.config'),
        (ROOT/'docs/CSHARP-SHARE-PACKAGE.md','README.md')]:
        shutil.copyfile(source,a.staging/name)
    for folder in ('docs','licenses','test-images'):
        shutil.copytree(ROOT/folder,a.staging/folder,ignore=SKIP)
    for folder in ('winforms','csharp'):
        shutil.copytree(ROOT/'examples'/folder,a.staging/'examples'/folder,ignore=SKIP)
    shutil.copytree(ROOT/'models/onnx',a.staging/'models/onnx',ignore=SKIP)
    for file in ('LICENSE','NOTICE','RELEASE_VERSION','dependencies.lock.json'):
        shutil.copyfile(ROOT/file,a.staging/file)
    for file in ('Start-CSharp-Demo.bat','Start-CSharp-Demo-CPU-Preprocess.bat','Start-CSharp-Demo-GPU-DET-Experiment.bat','Start-CSharp-Demo-GPU-Preprocess-Experiment.bat','Start-CSharp-Demo-GPU-Crop-Experiment.bat','Check-GPU.bat'):
        shutil.copyfile(ROOT/'deploy/windows'/file,a.staging/file)
    (a.staging/'sdk/include').mkdir(parents=True)
    shutil.copyfile(ROOT/'include/lw_ppocr_vulkan.h',a.staging/'sdk/include/lw_ppocr_vulkan.h')
    prerequisites=a.staging/'prerequisites';prerequisites.mkdir()
    installer=prerequisites/f'VulkanRT-X64-{RUNTIME_VERSION}-Installer.exe'
    shutil.copyfile(a.runtime_installer,installer)
    prefix=f'VulkanRT-X64-{RUNTIME_VERSION}-Components/'
    with zipfile.ZipFile(a.runtime_components) as runtime:
        for member,target in [('x64/vulkan-1.dll',a.staging/'vulkan-1.dll'),
            ('VulkanRT-License.txt',prerequisites/'VulkanRT-License.txt')]:
            # Explicit file allowlist: no arbitrary zip extraction/driver binaries.
            with runtime.open(prefix+member) as source,target.open('wb') as out:
                shutil.copyfileobj(source,out)
    verify_models(a.staging)
    for name in ('lw.PPOCR.Vulkan.dll','vulkan-1.dll','lw-ppocr-vulkan-probe.exe',
                 'lw.PPOCR.Vulkan.CSharpDemo.exe','lw.PPOCR.Vulkan.WinFormsDemo.exe'):
        require_x64(a.staging/name)
    info=dict(package_kind='Windows x64 C# complete sharing preview',
        version=(ROOT/'RELEASE_VERSION').read_text().strip(),created_utc=datetime.now(timezone.utc).isoformat(),
        native_library_sha256=sha(dll),runtime_version=RUNTIME_VERSION,
        demo_revision=a.revision,native_qualification_sha256=sha(a.qualification),
        application_sha256={name:sha(a.staging/name) for name in ('lw.PPOCR.Vulkan.WinFormsDemo.exe','lw.PPOCR.Vulkan.CSharpDemo.exe','lw-ppocr-vulkan-probe.exe')},
        runtime_components_sha256=RUNTIME_ZIP_SHA,runtime_installer_sha256=RUNTIME_EXE_SHA,
        bundled_loader_sha256=sha(a.staging/'vulkan-1.dll'),
        runtime_source=f'https://sdk.lunarg.com/sdk/download/{RUNTIME_VERSION}/windows/',
        driver_bundled=False,driver_installed_by_package=False,
        native_system_imports={'lw.PPOCR.Vulkan.dll':['vulkan-1.dll','KERNEL32.dll'],
            'vulkan-1.dll':['CFGMGR32.dll','KERNEL32.dll','ADVAPI32.dll']},
        requirements='Windows x64 / .NET Framework >=4.0 / installed compatible Vulkan GPU vendor driver',
        model_variants=['tiny','small','medium'],precision='FP32 networks; FP64 image preprocessing on capable GPUs',
        gpu_preprocessing_default='auto: shaderFloat64 and no GPU profiling; otherwise CPU image preprocessing, still GPU inference',
        gpu_preprocess_launchers={'Start-CSharp-Demo.bat':'all three auto',
            'Start-CSharp-Demo-CPU-Preprocess.bat':'all three off; networks still GPU',
            'Start-CSharp-Demo-GPU-DET-Experiment.bat':'DET only',
            'Start-CSharp-Demo-GPU-Preprocess-Experiment.bat':'DET + CLS + REC; CPU crop',
            'Start-CSharp-Demo-GPU-Crop-Experiment.bat':'DET + CLS + REC + shared original/GPU crop'},
        note='Application-local loader is for portable testing; prefer system driver/runtime for long-term deployment. Not a driver bundle.')
    write_json(a.staging/'PACKAGE-INFO.json',info)
    write_json(a.staging/'validation/native-qualification.json',qualification)
    components=[dict(type='application',name='lw.PPOCR.Vulkan.CSharpDemo',version=info['version'],licenses=[dict(license=dict(id='Apache-2.0'))]),
        dict(type='library',name='lw.PPOCR.Vulkan',version=info['version'],licenses=[dict(license=dict(id='Apache-2.0'))],
             hashes=[dict(alg='SHA-256',content=sha(dll))]),
        dict(type='library',name='Vulkan-Loader',version=RUNTIME_VERSION,licenses=[dict(expression='Apache-2.0 AND MIT')],
             hashes=[dict(alg='SHA-256',content=sha(a.staging/'vulkan-1.dll'))])]
    for variant in ('tiny','small','medium'):
        for task in ('det','cls','rec'):
            components.append(dict(type='data',name=f'PP-OCRv6-{variant}-{task}',
                licenses=[dict(license=dict(id='Apache-2.0'))],
                hashes=[dict(alg='SHA-256',content=sha(a.staging/'models/onnx'/('ppocrv6-'+variant)/(task+'.onnx')))]))
    write_json(a.staging/'sbom/csharp-demo.cdx.json',dict(bomFormat='CycloneDX',specVersion='1.5',version=1,
        metadata=dict(timestamp=info['created_utc']),components=components))
    print('Prepared '+str(a.staging)+'; test this directory before archiving.')


def archive(a):
    info=json.loads((a.staging/'PACKAGE-INFO.json').read_text(encoding='utf-8'))
    verify_models(a.staging)
    if sha(a.staging/'lw.PPOCR.Vulkan.dll')!=info['native_library_sha256'] or sha(a.staging/'vulkan-1.dll')!=info['bundled_loader_sha256']:
        raise ValueError('runtime changed after preparation')
    for name,digest in info['application_sha256'].items():
        if sha(a.staging/name)!=digest:raise ValueError('application changed after preparation: '+name)
    if {p.name for p in a.staging.glob('*.dll')}!={'lw.PPOCR.Vulkan.dll','vulkan-1.dll'}:
        raise ValueError('unexpected runtime DLL; do not bundle benchmark/driver libraries')
    for name in ('Start-CSharp-Demo.bat','Start-CSharp-Demo-CPU-Preprocess.bat','Start-CSharp-Demo-GPU-DET-Experiment.bat','Start-CSharp-Demo-GPU-Preprocess-Experiment.bat','Start-CSharp-Demo-GPU-Crop-Experiment.bat','Check-GPU.bat','lw.PPOCR.Vulkan.WinFormsDemo.exe',
                 'lw.PPOCR.Vulkan.WinFormsDemo.exe.config','test-images/sample.jpg','prerequisites/VulkanRT-License.txt'):
        if not (a.staging/name).is_file():raise ValueError('missing package file: '+name)
    a.output.mkdir(parents=True,exist_ok=True)
    target=a.output/(a.staging.name+'.zip')
    if target.exists():raise FileExistsError('archive already exists; choose a new name')
    files=sorted(p for p in a.staging.rglob('*') if p.is_file() and p.name!='FILES.sha256')
    (a.staging/'FILES.sha256').write_text(''.join(f'{sha(p)}  {p.relative_to(a.staging).as_posix()}\n' for p in files),encoding='utf-8')
    with zipfile.ZipFile(target,'x',zipfile.ZIP_DEFLATED,compresslevel=6) as zip_out:
        for path in sorted(a.staging.rglob('*')):
            if path.is_file():zip_out.write(path,(Path(a.staging.name)/path.relative_to(a.staging)).as_posix())
    target.with_name(target.name+'.sha256').write_text(f'{sha(target)}  {target.name}\n',encoding='ascii')
    print('Created '+str(target)+'\nSHA-256: '+sha(target))


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('mode',choices=['prepare','archive'])
    p.add_argument('--build',type=Path,default=ROOT/'build/transfer-optimized')
    p.add_argument('--staging',type=Path,required=True)
    p.add_argument('--output',type=Path,default=ROOT/'dist')
    p.add_argument('--runtime-components',type=Path)
    p.add_argument('--runtime-installer',type=Path)
    p.add_argument('--qualification',type=Path,default=ROOT/'docs/reports/host-transfer/qualification.json')
    p.add_argument('--revision',default='timing-ui-opt1')
    a=p.parse_args()
    if a.mode=='prepare' and (not a.runtime_components or not a.runtime_installer):p.error('prepare requires runtime components and installer')
    if a.mode=='prepare':prepare(a)
    else:archive(a)
    return 0


if __name__=='__main__':raise SystemExit(main())
