"""Maintainer-only deterministic hash generation AFTER source review, never in CI."""
import hashlib
import argparse
import json
from pathlib import Path

root=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--write',action='store_true',help='Update selected assets AFTER review; preserve other locked components')
args=parser.parse_args()
paths=list((root/"third_party/simd-paddleocr/shaders").glob("*.comp"))
paths += [root/"third_party/nlohmann/json.hpp",root/"LICENSE",root/"licenses/nlohmann-json-MIT.txt",
          root/"licenses/PaddleOCR-models-APACHE-2.0.txt",root/"models/ppocrv6-tiny/det.onnx",
          root/"models/ppocrv6-tiny/det.json",root/"models/ppocrv6-tiny/weights.bin",root/"test-images/sample.jpg"]
paths += [root/"licenses/lw-PPOCR-C-MIT.txt"]
for task in ("cls", "rec"):
    paths += [root/f"models/ppocrv6-tiny/{task}/{name}" for name in ("source.onnx", "model.json", "weights.bin")]
paths += [root/"models/ppocrv6-tiny/rec/dictionary.txt"]
paths += list((root/"third_party/lw-ppocr-c").rglob("*.c")) + list((root/"third_party/lw-ppocr-c").rglob("*.h"))
files=[]
for path in sorted(paths,key=lambda x:x.relative_to(root).as_posix()):
    data=path.read_bytes()
    normalize=path.suffix in (".hpp",".txt", ".json", ".c", ".h", ".comp") or path.name=="LICENSE"
    if path.name == "dictionary.txt": normalize=False
    if normalize: data=data.replace(b"\r\n",b"\n").replace(b"\r",b"\n")
    files.append({"path":path.relative_to(root).as_posix(),"sha256":hashlib.sha256(data).hexdigest(),"normalize_lf":normalize})
# Never overwrite the complete evolving lock with the original bootstrap subset.
lockpath=root/'dependencies.lock.json'
lock=json.loads(lockpath.read_text(encoding='utf-8'))
replaced={item['path'] for item in files}
lock['files']=sorted([item for item in lock['files'] if item['path'] not in replaced]+files,key=lambda item:item['path'])
if args.write:
    lockpath.write_text(json.dumps(lock,indent=2,ensure_ascii=False)+'\n',encoding='utf-8')
print('WRITE' if args.write else 'DRY RUN (no files changed; --write requires review)',len(lock['files']),'total lock entries; regenerate SBOM explicitly after writing')
