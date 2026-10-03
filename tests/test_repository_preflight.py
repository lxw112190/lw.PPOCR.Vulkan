"""Unit-test first-push inventory boundaries without changing any Git state."""
import importlib.util
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock
import zipfile

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

class CSharpPackageTests(unittest.TestCase):
    """Synthetic packaging fixtures only; no GPU/model/PE qualification claim."""
    @classmethod
    def setUpClass(cls):
        source = path.parent/'package_csharp_demo.py'
        spec = importlib.util.spec_from_file_location('csharp_package', source)
        cls.package = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.package)

    def test_retained_launcher_policy(self):
        expected = {'Start-CSharp-Demo.bat': 'auto',
                    'Start-CSharp-Demo-CPU-Preprocess.bat': '0'}
        self.assertEqual(set(self.package.DEMO_LAUNCHERS), set(expected))
        folder = self.package.ROOT/'deploy/windows'
        self.assertFalse(list(folder.glob('Start-CSharp-Demo-GPU-*-Experiment.bat')))
        for name, value in expected.items():
            script = (folder/name).read_text(encoding='utf-8')
            for stage in ('DET', 'TEXT', 'CROP'):
                self.assertIn(f'set LWVK_GPU_{stage}_PREPROCESS={value}\n', script)

    def test_archive_without_removed_launchers_and_missing_retained_launcher(self):
        package = self.package
        with tempfile.TemporaryDirectory() as temp:
            staging = Path(temp)/'fixture'
            output = Path(temp)/'output'
            files = [*package.DEMO_LAUNCHERS, 'Check-GPU.bat',
                     'lw.PPOCR.Vulkan.dll', 'vulkan-1.dll',
                     'lw.PPOCR.Vulkan.WinFormsDemo.exe',
                     'lw.PPOCR.Vulkan.CSharpDemo.exe', 'lw-ppocr-vulkan-probe.exe',
                     'lw.PPOCR.Vulkan.WinFormsDemo.exe.config',
                     'test-images/sample.jpg', 'prerequisites/VulkanRT-License.txt']
            for name in files:
                target = staging/name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(b'synthetic package fixture')
            info = dict(native_library_sha256=package.sha(staging/'lw.PPOCR.Vulkan.dll'),
                        bundled_loader_sha256=package.sha(staging/'vulkan-1.dll'),
                        application_sha256={name: package.sha(staging/name)
                                            for name in files if name.endswith('.exe')})
            (staging/'PACKAGE-INFO.json').write_text(json.dumps(info), encoding='utf-8')
            args = SimpleNamespace(staging=staging, output=output)
            # Model hashes are covered separately; these fixtures exercise packaging policy.
            with mock.patch.object(package, 'verify_models'):
                for name in package.DEMO_LAUNCHERS:
                    target = staging/name
                    target.unlink()
                    with self.assertRaises(ValueError) as error:
                        package.archive(args)
                    self.assertEqual(str(error.exception), 'missing package file: '+name)
                    self.assertFalse(output.exists())
                    target.write_bytes(b'synthetic package fixture')
                package.archive(args)
            archive = output/'fixture.zip'
            with zipfile.ZipFile(archive) as contents:
                for name in package.DEMO_LAUNCHERS:
                    self.assertIn('fixture/'+name, contents.namelist())
                self.assertFalse(any('-Experiment.bat' in name for name in contents.namelist()))
            self.assertEqual(archive.with_name(archive.name+'.sha256').read_text().strip(),
                             package.sha(archive)+'  '+archive.name)


class WorkflowLayersTests(unittest.TestCase):
    """Static scope checks; do not claim Linux or GPU execution."""
    def test_daily_linux_keeps_host_checks_without_network_suite(self):
        workflow = (path.parents[1]/'.github/workflows/build.yml').read_text(encoding='utf-8')
        linux = workflow.split('\n  linux:\n', 1)[1]
        self.assertIn('timeout-minutes: 30', linux)
        for command in ('ctest --test-dir', '--allow-no-device',
                        'tests/test_abi_exports.py', 'tests/test_api.py',
                        'tests/test_http_host.py', 'scripts/supply_chain.py',
                        'scripts/package.py', 'scripts/verify_archive.py',
                        'install-service.sh --verify-only', 'ldd "$binary"'):
            self.assertIn(command, linux)
        for heavy in ('test_det_reference.py', 'test_text_reference.py',
                      'test_ocr_reference.py', 'tests/test_http.py',
                      'gpu_preprocess_probe', 'gpu_crop_probe',
                      'LWVK_SOFTWARE_GRAPH_TIMEOUT_MS', 'requirements-dev.txt'):
            self.assertNotIn(heavy, linux)

    def test_full_software_suite_is_manual_and_preserves_coverage(self):
        root = path.parents[1]
        full = (root/'.github/workflows/linux-software-validation.yml').read_text(encoding='utf-8')
        trigger = full.split('\non:\n', 1)[1].split('\npermissions:', 1)[0]
        self.assertEqual(trigger.strip(), 'workflow_dispatch:')
        self.assertIn('timeout-minutes: 90', full)
        self.assertIn("LWVK_SOFTWARE_GRAPH_TIMEOUT_MS: '180000'", full)
        self.assertIn('VK_LAYER_KHRONOS_validation', full)
        self.assertIn("VK_LAYER_VALIDATE_SYNC: '1'", full)
        for command in ('lwvk_det_gpu_preprocess_probe', 'lwvk_text_gpu_preprocess_probe',
                        'lwvk_gpu_crop_probe', 'tests/test_invalid_models.py',
                        'tests/test_http.py', '--quick --extended --iterations 2',
                        'scripts/verify_archive.py'):
            self.assertIn(command, full)
        for variant in ('tiny', 'small', 'medium'):
            self.assertIn(f'models/onnx/ppocrv6-{variant}', full)
            self.assertIn(f'onnx-{variant}-text.json', full)
            self.assertIn(f'onnx-{variant}-full.json', full)
        self.assertIn('if: always()', full)


if __name__ == '__main__':unittest.main()
