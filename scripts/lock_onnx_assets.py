"""Explicit maintainer update after source review; never run automatically in CI."""
import argparse,hashlib,json
from pathlib import Path
root=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--write',action='store_true',help='Update only AFTER reviewing model/shader changes')
args=parser.parse_args()
catalog=json.loads((root/'models/onnx/catalog.json').read_text(encoding='utf-8'))
for variant in ('tiny','small','medium'):
    for file,key in (('det.onnx','det_sha256'),('rec.onnx','rec_sha256'),('dictionary.txt','dictionary_sha256'),('cls.onnx','cls_sha256')):
        expected=catalog[key] if key=='cls_sha256' else catalog[variant][key]
        path=root/'models/onnx'/('ppocrv6-'+variant)/file
        assert hashlib.sha256(path.read_bytes()).hexdigest()==expected,(variant,file)
lockpath=root/'dependencies.lock.json';lock=json.loads(lockpath.read_text(encoding='utf-8'))
prefixes=('models/onnx/','third_party/simd-paddleocr/shaders/','third_party/simd-paddleocr/onnx-reference/')
entries=[v for v in lock['files'] if not v['path'].startswith(prefixes)]
for prefix in prefixes:
    for path in sorted((root/prefix).rglob('*')):
        if not path.is_file():continue
        raw=path.read_bytes();normalize=path.suffix in ('.cs','.comp','.json')
        if normalize:raw=raw.replace(b'\r\n',b'\n').replace(b'\r',b'\n')
        entries.append(dict(path=path.relative_to(root).as_posix(),sha256=hashlib.sha256(raw).hexdigest(),normalize_lf=normalize))
lock['files']=sorted(entries,key=lambda x:x['path'])
lock['onnx_reader_reference']=dict(repository='https://github.com/lxw112190/SimdPaddleOCR',commit='d94a79ec56ec0d9bec1cb7c45f91dc1081161080',license='Apache-2.0')
if args.write:
    lockpath.write_text(json.dumps(lock,indent=2,ensure_ascii=False)+'\n',encoding='utf-8')
print('WRITE' if args.write else 'DRY RUN (no files changed; --write requires review)',len(entries),'ONNX/shader lock entries; regenerate SBOM explicitly after writing')
