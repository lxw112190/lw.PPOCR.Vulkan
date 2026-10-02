"""Staged-service harness. Config, cwd and logs are isolated from the package."""
import contextlib
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import time
import urllib.error
import urllib.request
from contract_common import validate_response

def request(url,body=None,content_type='application/octet-stream',key=None):
    headers={'Content-Type':content_type}
    if key is not None: headers['X-API-Key']=key
    req=urllib.request.Request(url,data=body,headers=headers)
    try:
        response=urllib.request.urlopen(req,timeout=120)
    except urllib.error.HTTPError as error:
        response=error
    with response:
        data=response.read()
        return response.status,dict(response.headers),data

class Service:
    def __init__(self,package,output,device=0,**overrides):
        self.package=Path(package).resolve(); self.output=Path(output).resolve()
        self.output.mkdir(parents=True,exist_ok=True)
        self.exe=self.package/('lw-ppocr-vulkan-http-service.exe' if os.name=='nt' else 'lw-ppocr-vulkan-http-service')
        with socket.socket() as sock:
            sock.bind(('127.0.0.1',0)); port=sock.getsockname()[1]
        self.config=json.loads((self.package/'http-service.json').read_text(encoding='utf-8'))
        # Exercise the package's actual default model (currently official Tiny
        # ONNX), rather than silently substituting the legacy JSON/BIN export.
        model_root=(self.package/Path(self.config['model_root'])).resolve()
        self.config.update(port=port,device_index=device,model_root=str(model_root),
                           web_root=str(self.package/'www'),log_dir=str(self.output/'logs'))
        self.config.update(overrides)
        self.file=self.output/'config.json'; self.file.write_text(json.dumps(self.config),encoding='utf-8')
        self.url=f'http://127.0.0.1:{port}'; self.port=port; self.process=None
    def __enter__(self):
        self.stdout=open(self.output/'service-output.txt','w',encoding='utf-8')
        flags=subprocess.CREATE_NEW_PROCESS_GROUP if os.name=='nt' else 0
        env=os.environ.copy(); env.pop('LWVK_API_KEY',None)
        self.process=subprocess.Popen([str(self.exe),'--config',str(self.file)],cwd=self.output,
            stdout=self.stdout,stderr=subprocess.STDOUT,creationflags=flags,env=env)
        deadline=time.monotonic()+90
        while time.monotonic()<deadline:
            if self.process.poll() is not None: break
            try:
                if request(self.url+'/health')[0]==200: return self
            except (OSError,urllib.error.URLError): pass
            time.sleep(.1)
        self.__exit__(None,None,None)
        raise RuntimeError((self.output/'service-output.txt').read_text(encoding='utf-8',errors='replace'))
    def __exit__(self,*unused):
        if self.process and self.process.poll() is None:
            self.process.send_signal(signal.CTRL_BREAK_EVENT if os.name=='nt' else signal.SIGTERM)
            try: self.process.wait(timeout=90)
            except subprocess.TimeoutExpired:
                self.process.kill(); self.process.wait(); raise RuntimeError('Service graceful shutdown timed out')
        self.stdout.close()
        if self.process and self.process.returncode:
            raise RuntimeError(f'Service exit {self.process.returncode}; inspect {self.output}/service-output.txt')

def json_request(service,path,body=None,kind='application/octet-stream',key=None,expected=200):
    status,headers,data=request(service.url+path,body,kind,key)
    value=json.loads(data)
    validate_response(value)
    assert status==expected,(status,value)
    assert value['ok']==(status<400)
    assert value['api_version']==1
    assert value['request_id']==headers['X-Request-ID']
    return value
