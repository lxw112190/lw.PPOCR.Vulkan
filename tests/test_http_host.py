"""Configuration preflight works without creating a Vulkan device."""
import argparse,json,subprocess,tempfile
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--service',type=Path,required=True);a=p.parse_args()
with tempfile.TemporaryDirectory(prefix='lwvk-http-config-') as directory:
    path=Path(directory)/'config.json'
    for config,code,message in [({'schema_version':1},0,'Configuration valid'),({},1,'requires schema_version'),
        ({'schema_version':2},1,'schema_version'),({'schema_version':1,'port':1.5},1,'must be an integer'),
        ({'schema_version':1,'unknown':True},1,'unknown configuration field'),
        ({'schema_version':1,'worker_threads':0},1,'outside allowed range'),
        ({'schema_version':1,'det_limit_side':123},1,'multiple of 32')]:
        path.write_text(json.dumps(config),encoding='utf-8')
        result=subprocess.run([str(a.service.resolve()),'--check-config','--config',str(path)],capture_output=True,encoding='utf-8',errors='replace',timeout=10)
        assert result.returncode==code and message in result.stdout+result.stderr,(config,result)
print('PASS: HTTP config types, unknown fields, version/range rejection without GPU')
