"""Explicit reviewed HTTP vendor lock regeneration (never called automatically)."""
import hashlib
import json
from pathlib import Path
root=Path(__file__).resolve().parents[1]
path=root/'dependencies.lock.json'
lock=json.loads(path.read_text(encoding='utf-8'))
prefixes=('third_party/cpp-httplib/','third_party/spdlog/','third_party/stb/')
licenses=('licenses/cpp-httplib-MIT.txt','licenses/spdlog-MIT.txt','licenses/fmt-BSD.txt','licenses/stb-MIT-or-Unlicense.txt')
entries=[v for v in lock['files'] if not v['path'].startswith(prefixes) and v['path'] not in licenses and v['path']!='NOTICE']
paths=[root/'NOTICE']+[root/p for p in licenses]
for prefix in prefixes:
    paths.extend(p for p in (root/prefix).rglob('*') if p.is_file())
for p in sorted(paths):
    raw=p.read_bytes().replace(b'\r\n',b'\n').replace(b'\r',b'\n')
    entries.append({'path':p.relative_to(root).as_posix(),'sha256':hashlib.sha256(raw).hexdigest(),'normalize_lf':True})
lock['files']=sorted(entries,key=lambda v:v['path'])
lock['http_dependencies']={
 'cpp_httplib':{'version':'0.49.0','license':'MIT','repository':'https://github.com/yhirose/cpp-httplib','local_patch':'best-effort 429 on transport queue overflow'},
 'spdlog':{'version':'1.17.0','license':'MIT','repository':'https://github.com/gabime/spdlog'},
 'fmt':{'version':'12.1.0','license':'BSD-2-Clause','repository':'https://github.com/fmtlib/fmt'},
 'stb_image':{'version':'2.30','license':'MIT','repository':'https://github.com/nothings/stb','commit':'2c980bb59875b0d32144a71867fbdebb2f77cd20'}}
path.write_text(json.dumps(lock,indent=2,ensure_ascii=False)+'\n',encoding='utf-8')
print('Updated reviewed HTTP dependency lock:',len(entries),'files')
