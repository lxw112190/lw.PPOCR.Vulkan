"""Verify the exact sharing ZIP and summarize already completed live tests."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import statistics
import zipfile


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--archive',type=Path,required=True)
    p.add_argument('--reports',type=Path,required=True)
    p.add_argument('--qualification',type=Path)
    a=p.parse_args()
    digest=hashlib.sha256(a.archive.read_bytes()).hexdigest()
    assert a.archive.with_name(a.archive.name+'.sha256').read_text().split()[0]==digest
    prefix=a.archive.stem+'/'
    with zipfile.ZipFile(a.archive) as z:
        manifest=z.read(prefix+'FILES.sha256').decode('utf-8')
        names=set()
        for line in manifest.splitlines():
            expected,name=line.split('  ',1)
            assert name not in names and '..' not in Path(name).parts and not Path(name).is_absolute()
            assert hashlib.sha256(z.read(prefix+name)).hexdigest()==expected,name
            names.add(name)
        assert {x.filename for x in z.infolist() if not x.is_dir()}=={prefix+x for x in names}|{prefix+'FILES.sha256'}
        info=json.loads(z.read(prefix+'PACKAGE-INFO.json'))
        for name,expected in info['application_sha256'].items():
            assert hashlib.sha256(z.read(prefix+name)).hexdigest()==expected
        baseline='0cfd9b5bbc192193b3208392eaee1cdb9e7012202e5f944522a504570ea81894'
        if a.qualification:
            raw=a.qualification.read_bytes()
            assert info['native_library_sha256']==json.loads(raw)['library_sha256']
            assert info['native_qualification_sha256']==hashlib.sha256(raw).hexdigest()
            assert json.loads(z.read(prefix+'validation/native-qualification.json'))==json.loads(raw)
        else:
            assert info['native_library_sha256']==baseline
    console=json.loads((a.reports/'extracted/console.json').read_text(encoding='utf-8'))
    assert console['passed'] and console['checked_manifest_files']==len(names)
    assert {(r['device'],r['model']) for r in console['models_and_devices']}=={(d,m) for d in (0,1) for m in ('tiny','small','medium')}
    timings=[]
    for device in (0,1):
        r=json.loads((a.reports/f'extracted/winforms-device{device}.json').read_text(encoding='utf-8'))
        assert r['ok'] and r['full_items']==16 and r['lazy_details']=='JSON and grid passed'
        calls=r['measurements'];assert len(calls)==13
        for i,c in enumerate(calls):
            assert c['first_call']==(i==0)
            assert c['ui_ready_ms']>=c['client_ms']>=c['native_call_ms']>=c['timing']['total_ms']
        timings.append(dict(device=device,gpu=r['gpu'],warmup_calls=3,measured_calls=10,
            first_call=calls[0],warm_median_ms={k:statistics.median(c[k] for c in calls[3:])
                for k in ('client_ms','native_call_ms','ui_ready_ms')}))
    rebuilt=json.loads((a.reports/'source-rebuild/winforms-device1.json').read_text(encoding='utf-8'))
    assert rebuilt['ok'] and rebuilt['full_items']==16
    result=dict(passed=True,verified_utc=datetime.now(timezone.utc).isoformat(),archive=a.archive.name,
        archive_sha256=digest,archive_bytes=a.archive.stat().st_size,manifest_files=len(names),
        version=info['version'],demo_revision=info['demo_revision'],native_library_unchanged=info['native_library_sha256']==baseline,
        native_library_sha256=info['native_library_sha256'],application_sha256=info['application_sha256'],
        extracted_console=console,timings=timings,source_rebuild='Release x64 / real GPU full and ROI passed',
        preflight_exceptions='OpenCV-specific checks and HTTP/web assets do not apply to this Vulkan C#-only package.',
        limits='Local Windows 10 / two GPUs. Short repeat test, not a new long-run leak proof or controlled old/new speedup measurement.')
    target=a.archive.with_name(a.archive.name+'.validation.json')
    with target.open('x',encoding='utf-8') as out:json.dump(result,out,ensure_ascii=False,indent=2)
    print(json.dumps(dict(passed=True,sha256=digest,manifest_files=len(names),timings=timings),ensure_ascii=True))
    print('Created '+str(target))


if __name__=='__main__':main()
