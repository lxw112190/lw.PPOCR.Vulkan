"""Real staged Vulkan HTTP: binary/JSON/REC/batch/security/overload/recovery."""
import argparse
import base64
import concurrent.futures
import io
import http.client
import json
from pathlib import Path
import socket
import subprocess
import sys
import time
from PIL import Image
from http_common import Service,json_request,request
for stream in (sys.stdout,sys.stderr):
    if hasattr(stream,'reconfigure'): stream.reconfigure(encoding='utf-8',errors='backslashreplace')
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--package',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--device',type=int,default=0)
a=p.parse_args();key='local-http-smoke-secret';checks=[]
sample=(a.package/'test-images/sample.jpg').read_bytes()
with Image.open(io.BytesIO(sample)) as image:
    buf=io.BytesIO();image.crop((20,28,312,74)).save(buf,format='PNG');crop=buf.getvalue()
    buf=io.BytesIO();image.resize((501,499)).save(buf,format='BMP');bmp=buf.getvalue()
with Service(a.package,a.output/'normal',a.device,api_key=key,max_request_bytes=1048576,max_image_pixels=500000,max_batch_images=3,max_batch_total_pixels=100000,max_batch_decoded_bytes=300000) as s:
    for changes,expected in [({'schema_version':None},'schema_version'),({'typo':1},'unknown configuration field'),({'device_index':4294967295},'does not exist')]:
        config=dict(s.config);config.update(changes);path=s.output/'invalid-config.json';path.write_text(json.dumps(config),encoding='utf-8')
        proc=subprocess.run([str(s.exe),'--config',str(path)],capture_output=True,encoding='utf-8',errors='replace',timeout=30)
        assert proc.returncode and expected in proc.stdout+proc.stderr,(changes,proc.stdout,proc.stderr)
    checks.append('strict configuration / invalid GPU')
    json_request(s,'/api/ocr',sample,expected=401)
    json_request(s,'/api/ocr',sample,key='wrong',expected=401)
    info=json_request(s,'/api/info',key=key);assert info['device_index']==a.device
    value=json_request(s,'/api/ocr',sample,key=key)
    assert len(value['result']['items'])==16 and value['result']['items'][0]['text']=='纯臻营养护发素'
    assert all('box' not in item for item in value['result']['items'])
    encoded=json.dumps({'image_base64':base64.b64encode(sample).decode()}).encode()
    other=json_request(s,'/api/ocr',encoded,'application/json',key)
    assert [i['text'] for i in other['result']['items']]==[i['text'] for i in value['result']['items']]
    rec=json_request(s,'/api/recognize',crop,key=key);assert rec['result']['text']=='纯臻营养护发素'
    batch=json.dumps({'images_base64':[base64.b64encode(crop).decode()]*2}).encode()
    values=json_request(s,'/api/recognize',batch,'application/json',key)
    assert [i['text'] for i in values['result']['items']]==['纯臻营养护发素']*2
    json_request(s,'/api/ocr',bmp,key=key)
    checks.append('binary JPEG/PNG/BMP / JSON Base64 / full OCR / REC-only / sequential batch')
    for body,kind,status,code in [(b'{','application/json',400,'invalid_json'),(b'{}','application/json',400,'invalid_fields'),
        (b'{"image_base64":'+b'['*100+b'0'+b']'*100+b'}','application/json',400,'json_too_complex'),
        (json.dumps({'images_base64':['']*100}).encode(),'application/json',413,'json_too_complex'),
        (b'{"image_base64":"@@@@"}','application/json',400,'invalid_base64'),(b'garbage','image/png',422,'invalid_image'),
        (sample[:50],'image/jpeg',422,'invalid_image'),
        (b'', 'application/octet-stream',422,'invalid_image'),(sample,'text/plain',415,'unsupported_media_type'),
        (b'x'*1048577,'application/octet-stream',413,'request_too_large'),
        (json.dumps({'images_base64':['a']*4}).encode(),'application/json',413,'batch_too_large'),
        (json.dumps({'images_base64':[base64.b64encode(sample).decode()]}).encode(),'application/json',413,'batch_memory_limit')]:
        v=json_request(s,'/api/recognize',body,kind,key,status);assert v['error_code']==code,v
        json_request(s,'/health')
    with Image.new('RGB',(1000,1000),'white') as image:
        b=io.BytesIO();image.save(b,format='PNG');big=b.getvalue()
    v=json_request(s,'/api/ocr',big,key=key,expected=413);assert v['error_code']=='image_too_large'
    json_request(s,'/api/recognize',crop,key=key)
    for path in ('/','/app.js','/style.css','/sample.jpg','/sponsor.jpg'): assert request(s.url+path)[0]==200
    json_request(s,'/missing',expected=404)
    checks.append('invalid/oversized inputs, API Key, static assets and recovery')
logs=list((a.output/'normal/logs').glob('*.log*'))
assert logs
for path in logs:
    text=path.read_text(encoding='utf-8');assert key not in text and '纯臻营养护发素' not in text
for line in (a.output/'normal/logs/access.log').read_text(encoding='utf-8').splitlines():
    entry=json.loads(line);assert entry['timestamp'].endswith('Z') and entry['log_schema_version']==1 and entry['request_id']
checks.append('JSONL timestamps / request IDs / secret and OCR-text privacy / graceful shutdown')
with Service(a.package,a.output/'wait',a.device,engine_wait_timeout_ms=1,worker_threads=4) as s:
    def send(_): return request(s.url+'/api/ocr',sample)[0]
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool: statuses=list(pool.map(send,range(4)))
    assert 200 in statuses and 503 in statuses,statuses
    json_request(s,'/api/recognize',crop)
checks.append('engine wait timeout 503 / recovery')
with Service(a.package,a.output/'queue',a.device,worker_threads=1,max_queued_requests=1) as s:
    held=[]
    try:
        # First incomplete request occupies the worker, second occupies the queue.
        for _ in range(2):
            sock=socket.create_connection(('127.0.0.1',s.port));sock.sendall(b'GET /health HTTP/1.1\r\nHost: localhost\r\n');held.append(sock);time.sleep(.2)
        # Transport rejects before parsing a request. Do not send additional
        # unread bytes: closing over unread data may legitimately reset a TCP
        # connection, especially on Windows (documented best-effort delivery).
        # Inspect the immediate reject on accept, without hiding it with retries.
        with socket.create_connection(('127.0.0.1',s.port),timeout=10) as rejected:
            response=http.client.HTTPResponse(rejected);response.begin()
            assert response.status==429 and response.getheader('Connection')=='close'
            body=response.read();response.close()
            assert json.loads(body)['error_code']=='queue_full'
    finally:
        for sock in held: sock.close()
    time.sleep(.2);json_request(s,'/health')
checks.append('transport queue overflow 429 / bounded sockets / recovery')
with Service(a.package,a.output/'nolog',a.device,logging_enabled=False,access_log_enabled=False) as s:
    json_request(s,'/api/recognize',crop)
assert not (a.output/'nolog/logs').exists()
checks.append('file logs can be disabled')
report={'ok':True,'version':info['version'],'device':a.device,'device_name':info['device_name'],'checks':checks,'full_items':16,'title':value['result']['items'][0]['text']}
a.output.mkdir(parents=True,exist_ok=True);(a.output/'http-smoke.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
print(json.dumps(report,ensure_ascii=False,indent=2))
