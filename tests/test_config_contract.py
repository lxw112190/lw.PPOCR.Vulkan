"""Compare config-schema cases with the actual --check-config process; no Vulkan device needed."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile
from contract_common import CONFIG

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--service',type=Path,required=True);a=p.parse_args()
    cases=[('minimal',{'schema_version':1},True),('missing-version',{},False),('future-version',{'schema_version':2},False),
           ('unknown',{'schema_version':1,'typo':1},False)]
    for name,field in CONFIG.schema['properties'].items():
        if name=='schema_version':continue
        for tag,value in [('default',field['default']),('null',None)]:
            cases.append((name+'-'+tag,dict(schema_version=1,**{name:value}),tag=='default'))
        if field['type'] in ('integer','number'):
            for tag,value in [('below',field['minimum']-1),('above',field['maximum']+1)]:
                cases.append((name+'-'+tag,dict(schema_version=1,**{name:value}),False))
    cases.extend([('crop-relation',dict(schema_version=1,max_crop_pixels=100,max_total_crop_pixels=99),False),
                  ('nonmultiple',dict(schema_version=1,det_limit_side=123),False),
                  ('nul',dict(schema_version=1,model_root='a\0b'),False),
                  ('float-integer-token',dict(schema_version=1,port=8787.0),False),
                  ('key-utf8-bytes',dict(schema_version=1,api_key='é'*513),False)])
    env=os.environ.copy();env.pop('LWVK_API_KEY',None)
    with tempfile.TemporaryDirectory(prefix='lwvk-contract-') as directory:
        config=Path(directory)/'config.json'
        for name,value,valid in cases:
            # The runtime adds cross-field/UTF-8-byte/integer-token constraints
            # that standard JSON Schema cannot express; these are documented.
            if name not in ('crop-relation','float-integer-token','key-utf8-bytes'):
                assert CONFIG.is_valid(value)==valid,(name,value)
            config.write_text(json.dumps(value),encoding='utf-8')
            r=subprocess.run([str(a.service.resolve()),'--check-config','--config',str(config)],
                             capture_output=True,encoding='utf-8',errors='replace',timeout=10,env=env)
            assert r.returncode==(0 if valid else 1),(name,r.returncode,r.stdout,r.stderr)
            assert ('Configuration valid' in r.stdout)==valid,(name,r.stdout,r.stderr)
            assert 'initializing single Vulkan' not in r.stdout+r.stderr
    print(f'PASS: {len(cases)} config/schema/native process cases; no GPU initialization')

if __name__=='__main__':main()
