"""Fault injection for source inventory, license completeness and SBOM drift."""
import copy
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import supply_chain as sc
from jsonschema import ValidationError


class SupplyChainTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.lock = json.loads((sc.ROOT / 'dependencies.lock.json').read_text(encoding='utf-8'))
        cls.version = (sc.ROOT / 'RELEASE_VERSION').read_text().strip()

    def test_source_and_schema(self):
        self.assertTrue(sc.audit(sc.ROOT)['passed'])

    def test_deterministic_bom_and_order_independent_files(self):
        wanted = sc.make_bom(self.lock, self.version)
        self.assertEqual(wanted, sc.make_bom(self.lock, self.version))
        self.assertNotEqual(wanted, sc.make_bom(self.lock, '99.0.0-test'))
        # Path tree digests are stable even when reviewed file entries reorder.
        reordered = copy.deepcopy(self.lock)
        reordered['files'].reverse()
        self.assertEqual(wanted['components'], sc.make_bom(reordered, self.version)['components'])

    def test_duplicate_and_unsafe_paths(self):
        for name in ('../outside', '/root', 'C:/root', 'a\\b', 'a/../b', './a', 'a//b', ''):
            value = {'lock_version': 1, 'files': [dict(path=name, sha256='a' * 64)]}
            with self.subTest(name=name), self.assertRaises(ValueError):
                sc.checked_entries(value)
        value = copy.deepcopy(self.lock)
        value['files'].append(value['files'][0])
        with self.assertRaisesRegex(ValueError, 'duplicate'):
            sc.checked_entries(value)

    def test_missing_tampered_license_and_raw_dictionary(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            license_path = root / 'licenses/example.txt'
            dictionary = root / 'models/dictionary.txt'
            license_path.parent.mkdir()
            dictionary.parent.mkdir()
            lock = {'lock_version': 1, 'files': [
                dict(path='licenses/example.txt', sha256=sc.digest(b'MIT\n'), normalize_lf=True),
                dict(path='models/dictionary.txt', sha256=sc.digest(b'a\nb\n'), normalize_lf=False)]}
            with self.assertRaisesRegex(ValueError, 'missing'):
                sc.verify_files(root, lock, source=False)
            license_path.write_bytes(b'MIT\r\n')
            dictionary.write_bytes(b'a\nb\n')
            sc.verify_files(root, lock, source=False)
            dictionary.write_bytes(b'a\r\nb\r\n')
            with self.assertRaisesRegex(ValueError, 'checksum'):
                sc.verify_files(root, lock, source=False)
            dictionary.write_bytes(b'a\nb\n')
            license_path.write_bytes(b'changed')
            with self.assertRaisesRegex(ValueError, 'checksum'):
                sc.verify_files(root, lock, source=False)

    def test_unknown_vendor_file_and_version_marker(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'third_party').mkdir()
            (root / 'third_party/new.h').write_text('unreviewed')
            with self.assertRaisesRegex(ValueError, 'unlocked'):
                sc.verify_files(root, {'lock_version': 1, 'files': []})
        value = copy.deepcopy(self.lock)
        value['http_dependencies']['cpp_httplib']['version'] = '99.0.0'
        with self.assertRaisesRegex(ValueError, 'marker'):
            sc.verify_markers(sc.ROOT, value)

    def test_component_completeness_and_license(self):
        value = copy.deepcopy(self.lock)
        value['files'].append(dict(path='third_party/unreviewed/source.c', sha256='a' * 64))
        with self.assertRaisesRegex(ValueError, 'omits'):
            sc.make_bom(value, self.version)
        value = copy.deepcopy(self.lock)
        value['files'] = [e for e in value['files'] if e['path'] != 'licenses/fmt-MIT.txt']
        with self.assertRaisesRegex(ValueError, 'license'):
            sc.make_bom(value, self.version)

    def test_schema_rejects_fake_format_and_license(self):
        for key, replacement in (('bomFormat', 'not-a-bom'), ('specVersion', '0')):
            value = sc.make_bom(self.lock, self.version)
            value[key] = replacement
            with self.subTest(key=key), self.assertRaises((ValidationError, ValueError)):
                sc.validate_bom(sc.ROOT, value)
        value = sc.make_bom(self.lock, self.version)
        value['components'][0]['licenses'][0]['license']['id'] = 'NOT-A-LICENSE'
        with self.assertRaises(ValidationError):
            sc.validate_bom(sc.ROOT, value)

    def test_default_release_metadata_gate(self):
        info = dict(metadata_version=1, version=self.version, system='Windows',
                    processor='AMD64', pointer_bytes=8, configuration='Release',
                    sanitizers='OFF', experimental_coop='OFF', experimental_rec_lanes='OFF')
        sc.validate_release_build(info, self.version, 'windows-x64')
        for key, bad in (('version', '0.1.0'), ('system', 'Linux'), ('processor', 'ARM64'),
                         ('pointer_bytes', 4), ('configuration', 'Debug'), ('metadata_version', 2),
                         ('sanitizers', 'ON'), ('experimental_coop', 'ON'), ('experimental_rec_lanes', 'ON')):
            value = dict(info, **{key: bad})
            with self.subTest(key=key), self.assertRaisesRegex(ValueError, 'metadata'):
                sc.validate_release_build(value, self.version, 'windows-x64')
        sc.validate_release_build(dict(info, system='Linux', processor='x86_64'), self.version, 'linux-x64')

    def test_license_metadata_mismatch_and_sbom_drift(self):
        value = copy.deepcopy(self.lock)
        value['http_dependencies']['cpp_httplib']['license'] = 'BSD-2-Clause'
        with self.assertRaisesRegex(ValueError, 'license'):
            sc.verify_markers(sc.ROOT, value)
        # Altering a valid SBOM is detected even if the official schema accepts it.
        original = sc.make_bom(self.lock, self.version)
        changed = copy.deepcopy(original)
        changed['components'][0]['name'] = 'wrong component name'
        sc.validate_bom(sc.ROOT, changed)
        validate = sc.validate_bom
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'dependencies.lock.json').write_text(json.dumps(self.lock), encoding='utf-8')
            (root / 'RELEASE_VERSION').write_text(self.version, encoding='utf-8')
            (root / 'SBOM.cdx.json').write_text(json.dumps(changed), encoding='utf-8')
            # Isolate the comparison after file verification; use the real
            # offline schema, without copying >200 MB of models into a fixture.
            with mock.patch.object(sc, 'verify_files', return_value=sc.checked_entries(self.lock)), \
                 mock.patch.object(sc, 'validate_bom', side_effect=lambda r, b, s: validate(sc.ROOT, b)):
                with self.assertRaisesRegex(ValueError, 'SBOM differs'):
                    sc.audit(root)


if __name__ == '__main__':
    unittest.main()
