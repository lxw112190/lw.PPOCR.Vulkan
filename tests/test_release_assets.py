"""Offline Release upload safety tests; all GitHub calls are mocked."""
import hashlib
import io
import json
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest import mock
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import upload_release_assets as release


def fixture(folder, version='1.0.0'):
    for platform, suffix in [('windows-x64', 'zip'), ('linux-x64', 'tar.gz')]:
        payload = b'tested package fixture'
        manifest = dict(manifest_version=1, version=version, platform=platform, contract_status='frozen',
                        files={'payload': dict(bytes=len(payload), sha256=hashlib.sha256(payload).hexdigest())})
        entries = {'staging/payload': payload, 'staging/PACKAGE-MANIFEST.json': json.dumps(manifest).encode()}
        preview = '-preview' if '-' in version else ''
        path = folder / f'lw.PPOCR.Vulkan-v{version}-{platform}-full-ocr{preview}.{suffix}'
        if suffix == 'zip':
            with zipfile.ZipFile(path, 'w') as archive:
                for name, value in entries.items(): archive.writestr(name, value)
        else:
            with tarfile.open(path, 'w:gz') as archive:
                for name, value in entries.items():
                    entry = tarfile.TarInfo(name); entry.size = len(value)
                    archive.addfile(entry, io.BytesIO(value))
        path.with_name(path.name + '.sha256').write_text(f'{release.digest(path)}  {path.name}\n', encoding='ascii')


class ReleaseTests(unittest.TestCase):
    def test_good_stable_and_prerelease(self):
        for version in ('1.0.0', '1.1.0-rc.1'):
            with tempfile.TemporaryDirectory() as temp:
                folder = Path(temp); fixture(folder, version)
                self.assertEqual(len(release.validate_assets(folder, 'v' + version)), 4)

    def test_bad_tag_extra_diagnostics_missing_checksum_and_corruption(self):
        with tempfile.TemporaryDirectory() as temp:
            folder = Path(temp); fixture(folder)
            for tag in ('main', 'v1.0', 'v1.0.0;echo', 'v1.1.0', '--clobber'):
                with self.assertRaises(ValueError): release.validate_assets(folder, tag)
            extra = folder / 'diagnostics.json'; extra.write_text('{}')
            with self.assertRaises(ValueError): release.validate_assets(folder, 'v1.0.0')
            extra.unlink()
            path = next(folder.glob('*.zip.sha256')); original = path.read_text()
            path.unlink()
            with self.assertRaises(ValueError): release.validate_assets(folder, 'v1.0.0')
            path.write_text('0' * 64 + '  ' + path.name.removesuffix('.sha256'))
            with self.assertRaises(AssertionError): release.validate_assets(folder, 'v1.0.0')
            path.write_text(original)

    def test_validation_failure_has_no_network_calls(self):
        with tempfile.TemporaryDirectory() as temp, mock.patch.object(release, 'gh') as cli:
            with self.assertRaises(ValueError): release.upload(Path(temp), 'v1.0.0', 'owner/repo')
            cli.assert_not_called()

    def test_new_release_is_draft_and_never_creates_a_tag(self):
        with tempfile.TemporaryDirectory() as temp:
            folder = Path(temp); fixture(folder)
            not_found = subprocess.CompletedProcess([], 1, '', 'gh: Not Found (HTTP 404)')
            success = subprocess.CompletedProcess([], 0, '', '')
            with mock.patch.object(release, 'gh', side_effect=[not_found, success, success]) as cli:
                release.upload(folder, 'v1.0.0', 'owner/repo')
            commands = [c.args for c in cli.call_args_list]
            self.assertIn('--draft', commands[1]); self.assertIn('--verify-tag', commands[1])
            self.assertEqual(commands[2][:3], ('release', 'upload', 'v1.0.0'))
            self.assertNotIn('--clobber', commands[2])

    def test_api_failure_is_not_mistaken_for_missing_release(self):
        with tempfile.TemporaryDirectory() as temp:
            folder = Path(temp); fixture(folder)
            with mock.patch.object(release, 'gh', return_value=subprocess.CompletedProcess([], 1, '', 'HTTP 403')) as cli:
                with self.assertRaises(RuntimeError): release.upload(folder, 'v1.0.0', 'owner/repo')
                self.assertEqual(cli.call_count, 1)

    def test_existing_assets_are_preserved_and_conflicts_block_all_writes(self):
        with tempfile.TemporaryDirectory() as temp:
            folder = Path(temp); fixture(folder)
            assets = [dict(name=p.name, digest='sha256:' + release.digest(p)) for p in folder.iterdir()]
            def response():
                return subprocess.CompletedProcess([], 0, json.dumps(dict(tag_name='v1.0.0', assets=assets)), '')
            with mock.patch.object(release, 'gh', return_value=response()) as cli:
                release.upload(folder, 'v1.0.0', 'owner/repo')
                self.assertEqual(cli.call_count, 1)
            assets.pop(); assets[0]['digest'] = 'sha256:' + '0' * 64
            with mock.patch.object(release, 'gh', return_value=response()) as cli:
                with self.assertRaisesRegex(ValueError, 'not replacing'): release.upload(folder, 'v1.0.0', 'owner/repo')
                self.assertEqual(cli.call_count, 1)

    def test_old_asset_without_digest_is_downloaded_and_compared(self):
        with tempfile.TemporaryDirectory() as temp:
            folder = Path(temp); fixture(folder); old = next(folder.glob('*.zip'))
            response = subprocess.CompletedProcess([], 0, json.dumps(dict(tag_name='v1.0.0', assets=[dict(name=old.name)])), '')
            def cli(*args, **kwargs):
                if args[0] == 'api': return response
                if args[:2] == ('release', 'download'):
                    target = Path(args[args.index('--dir') + 1]) / old.name
                    target.write_bytes(old.read_bytes())
                return subprocess.CompletedProcess([], 0, '', '')
            with mock.patch.object(release, 'gh', side_effect=cli) as calls:
                release.upload(folder, 'v1.0.0', 'owner/repo')
            upload = calls.call_args_list[-1].args
            self.assertNotIn(str(old.resolve()), upload)
            self.assertEqual(upload[:2], ('release', 'upload'))

    def test_workflow_limits_permissions_events_and_artifacts(self):
        # Keep upload tests standard-library-only, including on a clean runner.
        # These literal policy guards complement YAML/shell parsing before commit.
        value = (ROOT / '.github/workflows/build.yml').read_text(encoding='utf-8')
        self.assertIn('\npermissions:\n  contents: read\n', value)
        job = value.split('\n  release_assets:\n', 1)[1]
        self.assertIn('    needs: [windows, linux]\n', job)
        self.assertIn('    permissions:\n      contents: write\n', job)
        condition = job.split('    if: >-\n', 1)[1].split('    runs-on:', 1)[0]
        self.assertNotIn('always()', condition); self.assertNotIn('pull_request', condition)
        self.assertIn("github.event_name == 'push'", condition)
        self.assertIn("github.event_name == 'workflow_dispatch'", condition)
        for name in ('windows', 'linux'):
            artifact = name + '-x64-full-ocr'
            self.assertIn('          name: ' + artifact + '\n          path: release-assets\n', job)
            package = value.split('          name: ' + artifact + '\n', 1)[1].split('      - ', 1)[0]
            self.assertNotIn('build/reports', package)


if __name__ == '__main__':
    unittest.main()
