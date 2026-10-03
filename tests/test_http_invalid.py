"""Bounded hostile HTTP corpus followed by actual REC recovery, no ctypes/LD_PRELOAD."""
import argparse
import io
import json
from pathlib import Path
import random
from PIL import Image
from http_common import Service, json_request
from contract_common import validate_access

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--package', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--device', type=int, default=0)
    p.add_argument('--iterations', type=int, default=200)
    a=p.parse_args()
    if not 1 <= a.iterations <= 2000:
        p.error('iterations must be 1..2000')
    sample=(a.package/'test-images/sample.jpg').read_bytes()
    with Image.open(io.BytesIO(sample)) as image:
        buf=io.BytesIO()
        image.crop((20,28,312,74)).save(buf, format='PNG')
        crop=buf.getvalue()
    def recover(service):
        value=json_request(service, '/api/recognize', crop)
        assert value['result']['text']=='纯臻营养护发素',value
    rng=random.Random(6102)
    checks=0
    with Service(a.package,a.output, a.device, max_request_bytes=65536, max_image_pixels=262144,
                 max_batch_images=3, max_batch_total_pixels=30000, max_batch_decoded_bytes=90000) as s:
        recover(s)
        cases=[
            (b'{', 'application/json', 400, 'invalid_json'),
            (b'{}', 'application/json', 400, 'invalid_fields'),
            (b'{"image_base64":false}', 'application/json', 400, 'invalid_fields'),
            (b'{"image_base64":"@@@@"}', 'application/json', 400, 'invalid_base64'),
            (sample[:50], 'image/jpeg', 422, 'invalid_image'),
            (crop[:32], 'image/png', 422, 'invalid_image'),
            (b'', 'application/octet-stream', 422, 'invalid_image'),
            (b'x'*65537, 'application/octet-stream', 413, 'request_too_large'),
            # Keep this body below request_bytes so batch count is the first
            # rejection, not a different (also correct) request-body limit.
            (json.dumps({'images_base64':['a']*4}).encode(),
             'application/json', 413, 'batch_too_large'),
        ]
        for body,kind,status,code in cases:
            value=json_request(s,'/api/recognize',body,kind,expected=status)
            assert value['error_code']==code,value
            recover(s); checks+=1
        for i in range(a.iterations):
            raw=bytes(rng.randrange(256) for _ in range(rng.randrange(1,513)))
            # Invalid UTF-8 JSON must never be interpreted as a valid document.
            if i%2:
                # Exclude accidental '{'/ '[' depth triggers: that separate
                # guard intentionally precedes JSON parsing in the service.
                value=json_request(s,'/api/recognize',b'{\xff'+raw.hex().encode('ascii'),'application/json',expected=400)
                assert value['error_code']=='invalid_json',value
            else:
                value=json_request(s,'/api/recognize',b'not-an-image'+raw,'application/octet-stream',expected=422)
                assert value['error_code']=='invalid_image',value
            checks+=1
            if (i+1)%25==0:
                recover(s)
        recover(s)
    for line in (a.output/'logs/access.log').read_text(encoding='utf-8').splitlines():
        validate_access(json.loads(line))
    report=dict(passed=True, hostile_requests=checks, seed=6102,
                recovery='actual REC after each fixed case and every 25 random cases',
                scope='bounded deterministic corpus, not exhaustive fuzzing or leak proof')
    (a.output/'invalid-inputs.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(report,indent=2))

if __name__=='__main__':
    main()
