"""Verify an archive and every packaged file without extracting or executing it."""
import argparse
import hashlib
import json
from pathlib import Path,PurePosixPath
import tarfile
import zipfile

def verify(path):
    checksum=path.with_name(path.name+'.sha256').read_text(encoding='ascii').split()
    assert len(checksum)==2 and checksum[1]==path.name,'Invalid checksum sidecar'
    assert hashlib.sha256(path.read_bytes()).hexdigest()==checksum[0],'Archive checksum mismatch'
    if path.suffix=='.zip':
        archive=zipfile.ZipFile(path);names=[i.filename for i in archive.infolist() if not i.is_dir()]
        opener=archive.open
    else:
        archive=tarfile.open(path,'r:gz')
        members=archive.getmembers()
        assert all(m.isfile() or m.isdir() for m in members),'Package contains links or special entries'
        names=[m.name for m in members if m.isfile()];opener=archive.extractfile
    with archive:
        assert len(names)==len(set(names)),'Duplicate archive entries'
        for name in names:
            parts=PurePosixPath(name)
            assert not parts.is_absolute() and '..' not in parts.parts and '\\' not in name,'Unsafe archive path'
        roots={PurePosixPath(n).parts[0] for n in names};assert len(roots)==1,'Expected one package root'
        prefix=next(iter(roots))+'/'
        with opener(prefix+'PACKAGE-MANIFEST.json') as stream:manifest=json.load(stream)
        assert manifest['manifest_version']==1 and manifest['contract_status'] in ('candidate','frozen')
        # Keep historical pre-1.0 archives verifiable, but reject unfrozen 1.x releases.
        if int(manifest['version'].split('.')[0]) >= 1 and '-' not in manifest['version']:
            assert manifest['contract_status']=='frozen','Stable 1.x archive requires frozen contracts'
        assert set(names)=={prefix+n for n in manifest['files']}|{prefix+'PACKAGE-MANIFEST.json'},'Archive file set differs from manifest'
        for name,entry in manifest['files'].items():
            digest=hashlib.sha256();size=0
            with opener(prefix+name) as stream:
                while True:
                    data=stream.read(1024*1024)
                    if not data:break
                    size+=len(data);digest.update(data)
            assert size==entry['bytes'] and digest.hexdigest()==entry['sha256'],f'Packaged file mismatch: {name}'
    return manifest

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--archive',type=Path,required=True);a=p.parse_args()
    value=verify(a.archive)
    print(f"PASS: archive SHA-256 and {len(value['files'])} payload files; {value['platform']} v{value['version']}")

if __name__=='__main__':main()
