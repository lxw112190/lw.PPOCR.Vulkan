"""Archive the already-installed technical-preview package, with SHA-256."""
import argparse
import hashlib
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
library="lw.PPOCR.Vulkan.dll" if a.platform=="windows-x64" else "liblw.PPOCR.Vulkan.so"
for name in (library,"models/ppocrv6-tiny/det.json","models/ppocrv6-tiny/weights.bin",
             "models/ppocrv6-tiny/cls/model.json","models/ppocrv6-tiny/cls/weights.bin",
             "models/ppocrv6-tiny/rec/model.json","models/ppocrv6-tiny/rec/weights.bin",
             "models/ppocrv6-tiny/rec/dictionary.txt",
             "test-images/sample.jpg","README.md","README_EN.md","LICENSE","NOTICE"):
    if not (root/name).is_file(): raise RuntimeError(f"missing package file: {name}")
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
             f"run-http-service.{suffix}",f"install-service.{suffix}",f"uninstall-service.{suffix}",f"restart-service.{suffix}",f"stop-service.{suffix}"):
    if not (root/name).is_file(): raise RuntimeError(f"missing HTTP deployment file: {name}")
if a.platform == "windows-x64":
    for name in ("lw.PPOCR.Vulkan.CSharpDemo.exe", "lw.PPOCR.Vulkan.WinFormsDemo.exe",
                 "lw.PPOCR.Vulkan.WinFormsDemo.exe.config"):
        if not (root/name).is_file(): raise RuntimeError(f"missing Windows demo: {name}; enable LWVK_BUILD_CSHARP_EXAMPLE and install the C# compiler")
name=f"lw.PPOCR.Vulkan-v{version}-{a.platform}-full-ocr-preview"
output=a.output.resolve(); output.mkdir(parents=True,exist_ok=True)
archive=Path(shutil.make_archive(str(output/name),"zip" if a.platform=="windows-x64" else "gztar",root_dir=root.parent,base_dir=root.name))
digest=hashlib.sha256(archive.read_bytes()).hexdigest()
archive.with_name(archive.name+".sha256").write_text(f"{digest}  {archive.name}\n",encoding="ascii")
print(f"Created {archive}\nSHA-256: {digest}")
