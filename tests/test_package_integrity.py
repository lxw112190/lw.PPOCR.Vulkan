"""Archive gate rejects corruption, unlisted/missing files and unsafe entries."""
import hashlib
import io
import json
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest
import zipfile

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from verify_archive import verify

class Integrity(unittest.TestCase):
    def archive(self,folder,kind='zip',change=None):
        payload=b'tested binary'
        manifest=dict(manifest_version=1,version='0.6.0-dev.1',platform='windows-x64',contract_status='candidate',
                      files={'library.dll':dict(bytes=len(payload),sha256=hashlib.sha256(payload).hexdigest())})
        entries={'staging/library.dll':payload,'staging/PACKAGE-MANIFEST.json':json.dumps(manifest).encode()}
        if change:change(entries)
        path=Path(folder)/('package.zip' if kind=='zip' else 'package.tar.gz')
        if kind=='zip':
            with zipfile.ZipFile(path,'w') as output:
                for name,data in entries.items():output.writestr(name,data)
        else:
            with tarfile.open(path,'w:gz') as output:
                for name,data in entries.items():
                    entry=tarfile.TarInfo(name);entry.size=len(data);output.addfile(entry,io.BytesIO(data))
        path.with_name(path.name+'.sha256').write_text(hashlib.sha256(path.read_bytes()).hexdigest()+'  '+path.name+'\n')
        return path
    def test_good_formats(self):
        with tempfile.TemporaryDirectory() as d:
            for kind in ('zip','tar'):self.assertEqual(verify(self.archive(d,kind))['version'],'0.6.0-dev.1')
    def test_corrupt_payload(self):
        with tempfile.TemporaryDirectory() as d:
            path=self.archive(d,change=lambda e:e.update({'staging/library.dll':b'changed'}))
            with self.assertRaises(AssertionError):verify(path)
    def test_extra_missing_unsafe(self):
        with tempfile.TemporaryDirectory() as d:
            for change in (lambda e:e.update({'staging/extra':b'x'}),lambda e:e.pop('staging/library.dll'),
                           lambda e:e.update({'staging/../escape':b'x'})):
                with self.assertRaises(AssertionError):verify(self.archive(d,change=change))
    def test_sidecar(self):
        with tempfile.TemporaryDirectory() as d:
            path=self.archive(d);path.with_name(path.name+'.sha256').write_text('0'*64+'  '+path.name+'\n')
            with self.assertRaises(AssertionError):verify(path)

if __name__=='__main__':unittest.main()
