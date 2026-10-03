"""Frozen v1 contracts. --check never rewrites; --write requires explicit compatibility review."""
import argparse
import hashlib
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
DRAFT = 'https://json-schema.org/draft/2020-12/schema'
SYMBOLS = ['lwvk_version', 'lwvk_last_error', 'lwvk_device_count', 'lwvk_device_get',
    'lwvk_detector_create', 'lwvk_detector_destroy', 'lwvk_detector_run', 'lwvk_network_create',
    'lwvk_network_destroy', 'lwvk_network_shape', 'lwvk_network_run', 'lwvk_recognize_tensor',
    'lwvk_recognize_bgr', 'lwvk_ocr_config_default', 'lwvk_ocr_create', 'lwvk_ocr_destroy',
    'lwvk_ocr_run_bgr', 'lwvk_ocr_result_json', 'lwvk_ocr_result_destroy']

def obj(properties, required=None):
    return dict(type='object', properties=properties, required=list(properties) if required is None else required,
                additionalProperties=False)

def integer(lo, hi, default=None):
    result = dict(type='integer', minimum=lo, maximum=hi)
    if default is not None: result['default'] = default
    return result

def string(default=None):
    result = dict(type='string', pattern=r'^[^\u0000]*$')
    if default is not None: result['default'] = default
    return result

def config_schema():
    props = {'schema_version': dict(type='integer', const=1)}
    for name, lo, hi, default in [
        ('port',1,65535,8787), ('device_index',0,4294967295,0), ('worker_threads',1,32,4),
        ('max_queued_requests',1,128,32), ('engine_wait_timeout_ms',1,60000,5000),
        ('max_request_bytes',1024,67108864,20971520), ('max_image_pixels',1,40000000,40000000),
        ('max_batch_images',1,256,32), ('max_batch_total_pixels',1,100000000,40000000),
        ('max_batch_decoded_bytes',3,300000000,120000000), ('max_decode_work_bytes',1048576,536870912,268435456),
        ('log_max_bytes',1024,104857600,10485760), ('log_files',1,20,3),
        ('det_limit_side',32,960,960), ('max_workspace_bytes',0,1073741824,0),
        ('max_crop_pixels',1,8000000,4000000), ('max_total_crop_pixels',1,64000000,32000000)]:
        props[name] = integer(lo,hi,default)
    props['det_limit_side']['multipleOf'] = 32
    for name, default in [('listen_host','127.0.0.1'), ('model_root','models/ppocrv6-tiny'),
                          ('web_root','www'), ('log_dir','logs'), ('api_key','')]:
        props[name] = string(default)
    props['listen_host']['minLength'] = 1
    props['api_key']['maxLength'] = 1024
    for name, default in [('logging_enabled',True), ('access_log_enabled',True),
                          ('enable_classifier',True), ('use_dilation',False)]:
        props[name] = dict(type='boolean', default=default)
    for name, lo, hi, default in [('bitmap_threshold',0,1,.3), ('box_threshold',0,1,.6),
                                  ('unclip_ratio',.1,5,1.5), ('cls_threshold',0,1,.9)]:
        props[name] = dict(type='number',minimum=lo,maximum=hi,default=default)
    return dict({'$schema':DRAFT, 'title':'HTTP config v1',
        '$comment':'Runtime additionally requires integer JSON tokens (not 1.0), finite numbers, api_key <=1024 UTF-8 bytes after LWVK_API_KEY override, and max_total_crop_pixels >= max_crop_pixels after defaults. Paths resolve relative to the config file. Schema defaults describe missing fields, not the shipped ONNX config.'},
        **obj(props,['schema_version']))

def response_schema():
    ms = dict(type='number',minimum=0)
    score = dict(type='number',minimum=0,maximum=1)
    item = {k:dict(type='number') for k in ('x1','y1','x2','y2','x3','y3','x4','y4')}
    item.update(text=dict(type='string'),score=score,det_score=score,
                cls_label=dict(integer(-1,1),description='-1 when classification is disabled; otherwise 0 or 1'),cls_score=score)
    rec = obj(dict(text=dict(type='string'),score=score,gpu_rec_ms=ms,
                   image_width=integer(1,20000),image_height=integer(1,20000)))
    full = obj(dict(items=dict(type='array',items={'$ref':'#/$defs/OcrItem'},maxItems=1000),
        image_width=integer(1,20000),image_height=integer(1,20000),det_width=integer(32,960),
        det_height=integer(32,960),classifier_enabled=dict(type='boolean'),
        timing=obj({k:ms for k in ('det_ms','cls_ms','rec_ms','total_ms')})))
    full['properties']['det_width']['multipleOf'] = 32
    full['properties']['det_height']['multipleOf'] = 32
    common = dict(ok=dict(type='boolean'),api_version=dict(type='integer',const=1),
                  request_id=dict(type='string',minLength=1))
    def envelope(operation, result):
        return obj(dict(common,ok=dict(const=True),operation=dict(const=operation),result=result,server_total_ms=ms))
    defs = dict(OcrItem=obj(item), OcrResult=full, RecognitionResult=rec,
        OcrSuccess=envelope('ocr',{'$ref':'#/$defs/OcrResult'}),
        RecognitionSuccess=envelope('recognize',{'$ref':'#/$defs/RecognitionResult'}),
        BatchSuccess=envelope('recognize_batch',obj(dict(items=dict(type='array',minItems=1,maxItems=256,
                                                                  items={'$ref':'#/$defs/RecognitionResult'})))),
        Health=obj(dict(common,status=dict(enum=['ready','unavailable']))),
        Info=obj(dict(common,ok=dict(const=True),version=dict(type='string'),device_index=integer(0,4294967295),
                      device_name=dict(type='string'),backend=dict(const='vulkan-fp32'),engine_instances=dict(const=1))),
        Error=obj(dict(common,ok=dict(const=False),error_code=dict(type='string',pattern='^[a-z][a-z0-9_]*$'),
                       error=dict(type='string'))))
    return {'$schema':DRAFT, 'title':'HTTP response v1', '$defs':defs,
            'oneOf':[{'$ref':'#/$defs/'+k} for k in ('OcrSuccess','RecognitionSuccess','BatchSuccess','Health','Info','Error')]}

def access_schema():
    props = dict(log_schema_version=dict(type='integer',const=1),
        timestamp=dict(type='string',pattern=r'^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z$'),
        request_id=dict(type='string'),peer_ip=dict(type='string'),method=dict(type='string'),path=dict(type='string'),
        status=integer(100,599),request_bytes=dict(type='integer',minimum=0),
        response_bytes=dict(type='integer',minimum=0),duration_ms=dict(type='number',minimum=0),
        error_code=dict(type='string',pattern='^[a-z][a-z0-9_]*$'))
    return dict({'$schema':DRAFT,'title':'Access JSONL v1',
                 '$comment':'One record per parsed HTTP request. Transport queue rejection precedes parsing and has no request ID/access record. runtime.log is human-readable and not governed by this schema.'},
                **obj(props,[k for k in props if k!='error_code']))

def openapi():
    ref = lambda name: {'$ref':'http-response-v1.schema.json#/$defs/'+name}
    headers = {k:dict(schema=dict(type='string')) for k in ('X-Request-ID','X-API-Version')}
    error = dict(description='Stable error_code; diagnostic text is not a client contract. Queue overflow at transport level may instead close the connection.',
                 headers=headers,content={'application/json':dict(schema=ref('Error'))})
    paths = {}
    for path, method, result in [('/health','get','Health'),('/api/info','get','Info'),
                                  ('/api/ocr','post','OcrSuccess'),('/api/recognize','post','RecognitionSuccess')]:
        responses = {'200':dict(description='Success',headers=headers,content={'application/json':dict(schema=ref(result))})}
        if path=='/api/recognize':
            responses['200']['content']['application/json']['schema']={'oneOf':[ref('RecognitionSuccess'),ref('BatchSuccess')]}
        responses.update({str(code):error for code in (400,401,413,415,422,429,500,503)})
        if path=='/health':
            responses = {k:responses[k] for k in ('200','503')}
            responses['503'] = dict(description='Engine unavailable or stopping, not necessarily busy',content={'application/json':dict(schema=ref('Health'))},headers=headers)
        operation = dict(operationId=path.strip('/').replace('/','_'),responses=responses,
                         security=[] if path=='/health' else [{'ApiKey':[]},{}])
        if method=='post':
            single = obj(dict(image_base64=dict(type='string',minLength=1)))
            js = single if path=='/api/ocr' else {'oneOf':[single,obj(dict(images_base64=dict(type='array',minItems=1,maxItems=256,items=dict(type='string',minLength=1))))]}
            content = {kind:dict(schema=dict(type='string',format='binary')) for kind in
                       ('application/octet-stream','image/jpeg','image/png','image/bmp')}
            content['application/json'] = dict(schema=js)
            operation['requestBody'] = dict(required=True,content=content)
        paths[path] = {method:operation}
    return dict(openapi='3.1.0', info=dict(title='lw.PPOCR.Vulkan HTTP API',version='1',
        description='Frozen HTTP API v1, released with lw.PPOCR.Vulkan v1.0.0. API Key requirements depend on configuration. Full OCR coordinates refer to decoded original pixels; no EXIF auto rotation. Network timings include host submit/wait/readback, not GPU timestamps.'),
        paths=paths,components=dict(securitySchemes=dict(ApiKey=dict(type='apiKey',**{'in':'header'},name='X-API-Key'))))

def abi():
    header = (ROOT/'include/lw_ppocr_vulkan.h').read_text(encoding='utf-8')
    tokens = re.sub(r'/\*.*?\*/|//[^\n]*','',header,flags=re.S)
    fingerprint = hashlib.sha256(re.sub(r'\s+','',tokens).encode()).hexdigest()
    device_names = ('struct_size','device_index','vendor_id','device_id','api_version','device_type','max_shared_memory_bytes','subgroup_size','name')
    config_names = ('struct_size','device_index','det_limit_side','max_candidates','enable_classifier','use_dilation','reading_order','reserved',
                    'max_workspace_bytes','max_crop_pixels','max_total_crop_pixels','bitmap_threshold','box_threshold','unclip_ratio','cls_threshold')
    return dict(contract_version=1,status='frozen',architecture='x64',calling_convention='cdecl',encoding='UTF-8',
        public_header_token_sha256=fingerprint, symbols=sorted(SYMBOLS),
        status_codes=dict(LWVK_OK=0,LWVK_INVALID_ARGUMENT=1,LWVK_UNAVAILABLE=2,LWVK_MODEL_ERROR=3,LWVK_RUNTIME_ERROR=4,LWVK_BUFFER_TOO_SMALL=5),
        structures=dict(lwvk_device_info=dict(size=288,alignment=4,offsets=dict(zip(device_names,list(range(0,32,4))+[32]))),
                        lwvk_ocr_config=dict(size=72,alignment=8,offsets=dict(zip(config_names,[0,4,8,12,16,20,24,28,32,40,48,56,60,64,68])))))

def documents():
    return {'http-service-config-v1.schema.json':config_schema(), 'http-response-v1.schema.json':response_schema(),
            'access-log-v1.schema.json':access_schema(), 'http-api-v1.openapi.json':openapi(), 'c-abi-v1.json':abi()}

def encoded(value):
    return (json.dumps(value,ensure_ascii=False,indent=2)+'\n').encode('utf-8')

def main():
    p = argparse.ArgumentParser(description=__doc__)
    modes = p.add_mutually_exclusive_group();modes.add_argument('--write',action='store_true');modes.add_argument('--check',action='store_true')
    a = p.parse_args(); folder=ROOT/'schemas'; docs=documents()
    lock = dict(contract_version=1,status='frozen',files={k:hashlib.sha256(encoded(v)).hexdigest() for k,v in docs.items()})
    docs['contracts-v1.lock.json'] = lock
    for name,value in docs.items():
        path=folder/name; data=encoded(value)
        if a.write:
            folder.mkdir(exist_ok=True);path.write_bytes(data)
        elif not path.is_file() or path.read_bytes().replace(b'\r\n',b'\n')!=data:
            raise SystemExit(f'Contract drift: {path}; review v1 compatibility before explicitly regenerating contracts. Never regenerate in CI.')
    version=(ROOT/'RELEASE_VERSION').read_text().strip()
    if not re.fullmatch(r'\d+\.\d+\.\d+(?:-[a-z0-9.]+)?',version): raise SystemExit('Invalid RELEASE_VERSION')
    print(f'PASS: {len(docs)-1} frozen v1 contracts / reviewed hashes / header fingerprint; version={version}')

if __name__=='__main__':main()
