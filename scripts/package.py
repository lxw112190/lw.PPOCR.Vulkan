"""Archive the already-installed technical-preview package, with SHA-256."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil

p=argparse.ArgumentParser(description=__doc__)
p.add_argument("--staging",type=Path,required=True)
p.add_argument("--output",type=Path,required=True)
p.add_argument("--platform",choices=["windows-x64","linux-x64"],required=True)
a=p.parse_args()
root=a.staging.resolve()
for entry in root.rglob("*"):
    if (entry.is_dir() and entry.name in ("__pycache__", "bin", "obj", ".vs")) or entry.suffix in (".pyc", ".pyo", ".user", ".suo"):
        raise RuntimeError(f"development cache must not be packaged: {entry}")
version=(root/"RELEASE_VERSION").read_text(encoding="utf-8").strip()
for name in ('http-service-config-v1.schema.json','http-response-v1.schema.json','access-log-v1.schema.json',
             'http-api-v1.openapi.json','c-abi-v1.json','contracts-v1.lock.json'):
    if not (root/'schemas'/name).is_file():raise RuntimeError(f'missing candidate contract: {name}')
contracts=json.loads((root/'schemas/contracts-v1.lock.json').read_text(encoding='utf-8'))
for name,expected in contracts['files'].items():
    actual=hashlib.sha256((root/'schemas'/name).read_bytes().replace(b'\r\n',b'\n')).hexdigest()
    if actual!=expected:raise RuntimeError(f'package contract checksum mismatch: {name}')
library="lw.PPOCR.Vulkan.dll" if a.platform=="windows-x64" else "liblw.PPOCR.Vulkan.so"
for name in (library,"models/ppocrv6-tiny/det.json","models/ppocrv6-tiny/weights.bin",
             "models/ppocrv6-tiny/cls/model.json","models/ppocrv6-tiny/cls/weights.bin",
             "models/ppocrv6-tiny/rec/model.json","models/ppocrv6-tiny/rec/weights.bin",
             "models/ppocrv6-tiny/rec/dictionary.txt",
             "test-images/sample.jpg","README.md","README_EN.md","LICENSE","NOTICE",
             "dependencies.lock.json","sdk/include/lw_ppocr_vulkan.h","docs/RELEASE-GATES.md"):
    if not (root/name).is_file(): raise RuntimeError(f"missing package file: {name}")
dependency_lock=json.loads((root/'dependencies.lock.json').read_text(encoding='utf-8'))
for entry in dependency_lock['files']:
    path=root/entry['path']
    # Vendored source files intentionally are not runtime dependencies, but
    # every locked model/license file that IS shipped must retain its hash.
    if path.is_file():
        data=path.read_bytes()
        if entry.get('normalize_lf'):data=data.replace(b'\r\n',b'\n').replace(b'\r',b'\n')
        if hashlib.sha256(data).hexdigest()!=entry['sha256']:raise RuntimeError(f'packaged pinned asset mismatch: {path}')
for variant in ('tiny','small','medium'):
    for file in ('det.onnx','cls.onnx','rec.onnx','dictionary.txt'):
        path=root/'models/onnx'/('ppocrv6-'+variant)/file
        if not path.is_file():raise RuntimeError(f'missing ONNX model: {path}')
catalog=__import__('json').loads((root/'models/onnx/catalog.json').read_text(encoding='utf-8'))
for variant in ('tiny','small','medium'):
    for file,key in (('det.onnx','det_sha256'),('rec.onnx','rec_sha256'),('cls.onnx','cls_sha256'),('dictionary.txt','dictionary_sha256')):
        expected=catalog[key] if key=='cls_sha256' else catalog[variant][key]
        path=root/'models/onnx'/('ppocrv6-'+variant)/file
        if hashlib.sha256(path.read_bytes()).hexdigest()!=expected:raise RuntimeError(f'ONNX asset checksum mismatch: {path}')
service="lw-ppocr-vulkan-http-service.exe" if a.platform=="windows-x64" else "lw-ppocr-vulkan-http-service"
suffix="bat" if a.platform=="windows-x64" else "sh"
for name in (service,"http-service.json","www/index.html","www/app.js","www/style.css","www/sponsor.jpg",
             f"run-http-service.{suffix}",f"install-service.{suffix}",f"uninstall-service.{suffix}",f"restart-service.{suffix}",f"stop-service.{suffix}",f"start-service.{suffix}"):
    if not (root/name).is_file(): raise RuntimeError(f"missing HTTP deployment file: {name}")
if a.platform == "windows-x64":
    for name in ("lw.PPOCR.Vulkan.CSharpDemo.exe", "lw.PPOCR.Vulkan.WinFormsDemo.exe",
                 "lw.PPOCR.Vulkan.WinFormsDemo.exe.config"):
        if not (root/name).is_file(): raise RuntimeError(f"missing Windows demo: {name}; enable LWVK_BUILD_CSHARP_EXAMPLE and install the C# compiler")
name=f"lw.PPOCR.Vulkan-v{version}-{a.platform}-full-ocr-preview"
output=a.output.resolve(); output.mkdir(parents=True,exist_ok=True)
inventory={}
for path in sorted(root.rglob('*')):
    if path.is_file() and path!=root/'PACKAGE-MANIFEST.json':
        inventory[path.relative_to(root).as_posix()]={'bytes':path.stat().st_size,'sha256':hashlib.sha256(path.read_bytes()).hexdigest()}
(root/'PACKAGE-MANIFEST.json').write_text(json.dumps(dict(manifest_version=1,version=version,platform=a.platform,
    contract_status='candidate',files=inventory),ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
archive=Path(shutil.make_archive(str(output/name),"zip" if a.platform=="windows-x64" else "gztar",root_dir=root.parent,base_dir=root.name))
digest=hashlib.sha256(archive.read_bytes()).hexdigest()
archive.with_name(archive.name+".sha256").write_text(f"{digest}  {archive.name}\n",encoding="ascii")
print(f"Created {archive}\nSHA-256: {digest}")
