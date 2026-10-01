"""Unit-test first-push inventory boundaries without changing any Git state."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

path = Path(__file__).resolve().parents[1]/'scripts/repository_preflight.py'
spec = importlib.util.spec_from_file_location('preflight',path)
preflight = importlib.util.module_from_spec(spec)
spec.loader.exec_module(preflight)

class InventoryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.original = preflight.ROOT
        preflight.ROOT = Path(self.temp.name)

    def tearDown(self):
        preflight.ROOT = self.original
        self.temp.cleanup()

    def add(self,name):
        target = preflight.ROOT/name
        target.parent.mkdir(parents=True,exist_ok=True)
        target.write_bytes(b'test fixture')

    def files(self):
        return {f.relative_to(preflight.ROOT).as_posix() for f in preflight.inventory()}

    def test_build_cache_and_ide_directories(self):
        for name in ('build/local/x.cpp','build-native/x.cpp','dist/source.cpp',
            '.cache/data.bin','.ci/sdk.txt','.git/config','examples/bin/x.cpp','tests/__pycache__/a.pyc'):
            self.add(name)
        self.add('src/source.cpp')
        self.assertEqual(self.files(),{'src/source.cpp'})

    def test_models_public_images_and_notices_remain(self):
        expected={'models/onnx/model.onnx','models/weights.bin','test-images/sample.jpg','licenses/NOTICE.txt'}
        for name in expected:self.add(name)
        self.assertEqual(self.files(),expected)

    def test_binary_outputs_and_logs_are_excluded(self):
        for name in ('x.dll','x.exe','x.pdb','x.obj','x.pyc','runtime.log','demo.user'):
            self.add(name)
        self.add('include/api.h')
        self.assertEqual(self.files(),{'include/api.h'})

if __name__ == '__main__':unittest.main()
