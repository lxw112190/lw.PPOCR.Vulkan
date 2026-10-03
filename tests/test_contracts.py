"""Schema self-checks, positive/negative examples and OpenAPI local references."""
import copy
import json
import re
import unittest
from jsonschema import Draft202012Validator,ValidationError
from contract_common import ROOT,schema,CONFIG,HTTP,ACCESS

class Contracts(unittest.TestCase):
    def test_v1_release_freeze_metadata(self):
        version=(ROOT/'RELEASE_VERSION').read_text(encoding='utf-8').strip()
        self.assertEqual(version.split('.')[0],'1')
        for name in ('c-abi-v1.json','contracts-v1.lock.json'):
            value=json.loads((ROOT/'schemas'/name).read_text(encoding='utf-8'))
            self.assertEqual(value['contract_version'],1)
            self.assertEqual(value['status'],'frozen')
    def test_schemas_and_shipped_config(self):
        for v in (CONFIG,HTTP,ACCESS):Draft202012Validator.check_schema(v.schema)
        CONFIG.validate(json.loads((ROOT/'http-service.json').read_text()))
        source=(ROOT/'apps/http/config.cpp').read_text(encoding='utf-8')
        known=re.search(r'known\s*=\s*\{(.*?)\};',source,re.S).group(1)
        self.assertEqual(set(re.findall(r'"([a-z_]+)"',known)),set(CONFIG.schema['properties']))
    def test_config_invalid(self):
        for value in ({},{'schema_version':2},{'schema_version':True},{'schema_version':1,'typo':True},
                      {'schema_version':1,'port':0},{'schema_version':1,'det_limit_side':123}):
            with self.assertRaises(ValidationError):CONFIG.validate(value)
    def test_error_and_health(self):
        base=dict(api_version=1,request_id='test-1')
        HTTP.validate(dict(base,ok=False,error_code='invalid_image',error='cannot decode'))
        HTTP.validate(dict(base,ok=True,status='ready'))
        HTTP.validate(dict(base,ok=False,status='unavailable'))
        with self.assertRaises(ValidationError):HTTP.validate(dict(base,ok=False,error='missing stable code'))
    def test_full_ocr_no_duplicate_coordinates(self):
        item={k:10 for k in ('x1','y1','x2','y2','x3','y3','x4','y4')}
        item.update(text='测试',score=.9,det_score=.8,cls_label=0,cls_score=.99)
        result=dict(items=[item],image_width=320,image_height=320,det_width=320,det_height=320,
                    classifier_enabled=True,timing=dict(det_ms=1,cls_ms=2,rec_ms=3,total_ms=10))
        value=dict(ok=True,api_version=1,request_id='test-2',operation='ocr',server_total_ms=11,result=result)
        HTTP.validate(value)
        # Re-fitted rotated rectangles may extend outside the image. The Schema
        # deliberately promises numeric source coordinates, not clipped corners.
        item['x1']=-4.394726753234863;item['x2']=324.0
        HTTP.validate(value)
        item['cls_label']=-1;item['cls_score']=0;result['classifier_enabled']=False
        HTTP.validate(value)
        duplicate=copy.deepcopy(value);duplicate['result']['items'][0]['box']=[]
        with self.assertRaises(ValidationError):HTTP.validate(duplicate)
        result['timing']['total_ms']=-1
        with self.assertRaises(ValidationError):HTTP.validate(value)
    def test_recognition_and_batch(self):
        result=dict(text='example',score=.9,gpu_rec_ms=1,image_width=200,image_height=48)
        base=dict(ok=True,api_version=1,request_id='test-3',server_total_ms=2)
        HTTP.validate(dict(base,operation='recognize',result=result))
        HTTP.validate(dict(base,operation='recognize_batch',result=dict(items=[result,result])))
        with self.assertRaises(ValidationError):HTTP.validate(dict(base,operation='recognize_batch',result=dict(items=[])))
    def test_access_privacy_and_timestamp(self):
        value=dict(log_schema_version=1,timestamp='2026-10-02T10:01:02.003Z',request_id='test-4',peer_ip='127.0.0.1',
                   method='POST',path='/api/ocr',status=200,request_bytes=123,response_bytes=234,duration_ms=1)
        ACCESS.validate(value)
        for key in ('api_key','image_base64','text','authorization'):
            with self.assertRaises(ValidationError):ACCESS.validate(dict(value,**{key:'private'}))
        value['timestamp']='no timestamp'
        with self.assertRaises(ValidationError):ACCESS.validate(value)
    def test_openapi_local_refs(self):
        document=schema('http-api-v1.openapi.json');assert document['openapi']=='3.1.0'
        def walk(value):
            if isinstance(value,dict):
                if '$ref' in value:
                    name,fragment=value['$ref'].split('#',1);target=schema(name)
                    for key in fragment.strip('/').split('/'):target=target[key]
                for child in value.values():walk(child)
            elif isinstance(value,list):
                for child in value:walk(child)
        walk(document)
        self.assertEqual(set(document['paths']),{'/health','/api/info','/api/ocr','/api/recognize'})

if __name__=='__main__':unittest.main()
