"""Offline source/model SBOM audit. --write regenerates SBOM, NEVER asset hashes.

Normalized source fingerprints are not upstream Git or compiled-binary hashes.
The separate PACKAGE-MANIFEST inventories actual deployed bytes. SDK archives
are build inputs, not shipped dependencies; OS/driver inventories are excluded.
"""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import uuid
from jsonschema import Draft7Validator, FormatChecker
from referencing import Registry, Resource

ROOT = Path(__file__).resolve().parents[1]
SCHEMAS = ('bom-1.6.schema.json', 'spdx.schema.json', 'jsf-0.82.schema.json')
# Conversion input snapshots are not installed; runtime graphs/weights and the
# complete official ONNX model set remain mandatory. Never skip a suffix family.
CONVERSION_INPUTS = {'models/ppocrv6-tiny/det.onnx',
                     'models/ppocrv6-tiny/cls/source.onnx', 'models/ppocrv6-tiny/rec/source.onnx'}


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'), ensure_ascii=False)


def digest(data, normalize=False):
    if normalize:
        data = data.replace(b'\r\n', b'\n').replace(b'\r', b'\n')
    return hashlib.sha256(data).hexdigest()


def checked_entries(lock):
    if lock['lock_version'] != 1:
        raise ValueError('unsupported dependency lock version')
    entries = {}
    for entry in lock['files']:
        name = entry['path']
        path = PurePosixPath(name)
        if (not name or path.is_absolute() or path.as_posix() != name or
                '..' in path.parts or '\\' in name or ':' in name):
            raise ValueError('unsafe locked path: ' + name)
        if name in entries:
            raise ValueError('duplicate locked path: ' + name)
        if not re.fullmatch('[0-9a-f]{64}', entry['sha256']):
            raise ValueError('invalid SHA-256: ' + name)
        if not isinstance(entry.get('normalize_lf', False), bool):
            raise ValueError('invalid normalization flag: ' + name)
        entries[name] = entry
    return entries


def verify_files(root, lock, source=True):
    entries = checked_entries(lock)
    for name, entry in entries.items():
        if not source and name in CONVERSION_INPUTS:
            continue
        path = root / name
        if not source and name.startswith('third_party/'):
            if not name.startswith('third_party/cyclonedx/'):
                continue  # Vendored sources are compiled in, not shipped.
            path = root / 'metadata/cyclonedx' / PurePosixPath(name).name
        if not path.is_file() or path.is_symlink() or not path.resolve().is_relative_to(root.resolve()):
            raise ValueError('missing/unsafe pinned asset: ' + name)
        if digest(path.read_bytes(), entry.get('normalize_lf', False)) != entry['sha256']:
            raise ValueError('pinned asset checksum mismatch: ' + name)
    if source:
        for directory in ('third_party', 'licenses', 'models'):
            for path in (root / directory).rglob('*'):
                if path.is_symlink():
                    raise ValueError('unreviewed asset symlink: ' + str(path))
                if path.is_file() and path.relative_to(root).as_posix() not in entries:
                    raise ValueError('unlocked dependency/model/license: ' + str(path.relative_to(root)))
        verify_markers(root, lock)
    return entries


def verify_markers(root, lock):
    http = lock['http_dependencies']
    for key in ('cpp_httplib', 'spdlog', 'fmt', 'stb_image'):
        if http[key]['license'] != 'MIT':
            raise ValueError('review dependency license metadata: ' + key)
    for key, license_id in (('nlohmann_json', 'MIT'), ('lw_ppocr_c', 'MIT'),
                            ('simd_paddleocr', 'Apache-2.0'), ('onnx_reader_reference', 'Apache-2.0')):
        if lock[key]['license'] != license_id:
            raise ValueError('review dependency license metadata: ' + key)
    for key in ('lw_ppocr_c', 'simd_paddleocr', 'onnx_reader_reference'):
        if not re.fullmatch('[0-9a-f]{40}', lock[key]['commit']):
            raise ValueError('invalid pinned source commit: ' + key)
    markers = [
        ('third_party/cpp-httplib/httplib.h', 'CPPHTTPLIB_VERSION "' + http['cpp_httplib']['version'] + '"'),
        ('third_party/stb/stb_image.h', 'stb_image - v' + http['stb_image']['version']),
    ]
    for name, prefix, version in (
        ('third_party/spdlog/include/spdlog/version.h', 'SPDLOG_VER_', http['spdlog']['version']),
        ('third_party/nlohmann/json.hpp', 'NLOHMANN_JSON_VERSION_', lock['nlohmann_json']['version']),
    ):
        for suffix, number in zip(('MAJOR', 'MINOR', 'PATCH'), version.split('.')):
            markers.append((name, '#define ' + prefix + suffix + ' ' + number))
    a, b, c = map(int, http['fmt']['version'].split('.'))
    markers.append(('third_party/spdlog/include/spdlog/fmt/bundled/base.h', '#define FMT_VERSION ' + str(a * 10000 + b * 100 + c)))
    for name, marker in markers:
        if marker not in (root / name).read_text(encoding='utf-8'):
            raise ValueError('dependency version marker mismatch: ' + name)
    if http['fmt']['license'] != 'MIT' or 'Permission is hereby granted' not in (root / 'licenses/fmt-MIT.txt').read_text():
        raise ValueError('review fmt license metadata against original text')


def make_bom(lock, version):
    entries = checked_entries(lock)
    components, owned = [], set()
    http = lock['http_dependencies']

    def add(ref, name, revision, license_id, prefix, url, license_path, kind='library', exclude='\0'):
        paths = sorted(p for p in entries if p.startswith(prefix) and not p.startswith(exclude))
        if not paths or owned.intersection(paths):
            raise ValueError('empty/overlapping component: ' + ref)
        owned.update(paths)
        if license_path not in entries:
            raise ValueError('unlocked license: ' + license_path)
        tree = [{'path': p, 'sha256': entries[p]['sha256'], 'normalize_lf': entries[p].get('normalize_lf', False)} for p in paths]
        components.append(dict(type=kind, **{'bom-ref': ref}, name=name, version=revision,
            licenses=[{'license': {'id': license_id}}], externalReferences=[{'type': 'distribution', 'url': url}],
            properties=[{'name': 'lwvk:pinned-files', 'value': canonical(tree)},
                        {'name': 'lwvk:normalized-tree-sha256', 'value': digest(canonical(tree).encode())},
                        {'name': 'lwvk:license-file', 'value': license_path},
                        {'name': 'lwvk:artifact-role', 'value': 'validation-tooling' if ref == 'cyclonedx' else 'source-or-model'}]))

    fmt_prefix = 'third_party/spdlog/include/spdlog/fmt/bundled/'
    add('fmt', 'fmt (spdlog bundled snapshot)', http['fmt']['version'], 'MIT', fmt_prefix,
        http['fmt']['repository'], 'licenses/fmt-MIT.txt')
    add('spdlog', 'spdlog', http['spdlog']['version'], 'MIT', 'third_party/spdlog/',
        http['spdlog']['repository'], 'licenses/spdlog-MIT.txt', exclude=fmt_prefix)
    add('httplib', 'cpp-httplib (local queue-overflow patch)', http['cpp_httplib']['version'], 'MIT',
        'third_party/cpp-httplib/', http['cpp_httplib']['repository'], 'licenses/cpp-httplib-MIT.txt')
    add('stb', 'stb_image', http['stb_image']['commit'], 'MIT', 'third_party/stb/',
        http['stb_image']['repository'], 'licenses/stb-MIT-or-Unlicense.txt')
    add('json', 'nlohmann/json', lock['nlohmann_json']['version'], 'MIT', 'third_party/nlohmann/',
        'https://github.com/nlohmann/json', 'licenses/nlohmann-json-MIT.txt')
    for ref, key, name, prefix, license_path in (
        ('host-geometry', 'lw_ppocr_c', 'lw.PPOCR.C host snapshot', 'third_party/lw-ppocr-c/', 'licenses/lw-PPOCR-C-MIT.txt'),
        ('shaders', 'simd_paddleocr', 'Vulkan shader reference snapshot', 'third_party/simd-paddleocr/shaders/', 'LICENSE'),
        ('onnx-reader', 'onnx_reader_reference', 'ONNX reader reference snapshot', 'third_party/simd-paddleocr/onnx-reference/', 'LICENSE'),
    ):
        meta = lock[key]
        add(ref, name, meta['commit'], meta['license'], prefix, meta['repository'], license_path)
    add('cyclonedx', 'CycloneDX offline validation schemas', '1.6', 'Apache-2.0',
        'third_party/cyclonedx/', 'https://github.com/CycloneDX/specification/tree/1.6/schema', 'licenses/CycloneDX-APACHE-2.0.txt')
    for variant in ('tiny', 'small', 'medium'):
        add('model-' + variant, 'PP-OCRv6 ' + variant + ' ONNX + shared CLS + dictionary', 'reviewed-content-snapshot',
            'Apache-2.0', 'models/onnx/ppocrv6-' + variant + '/',
            'https://huggingface.co/PaddlePaddle/PP-OCRv6_' + variant + '_det_onnx',
            'licenses/PaddleOCR-models-APACHE-2.0.txt', kind='machine-learning-model')
    add('converted-tiny', 'PP-OCRv6 Tiny offline converted graphs + weights', 'format-0', 'Apache-2.0',
        'models/ppocrv6-tiny/', 'https://huggingface.co/PaddlePaddle/PP-OCRv6_tiny_det_onnx',
        'licenses/PaddleOCR-models-APACHE-2.0.txt', kind='machine-learning-model')
    components[-1]['properties'].append({'name': 'lwvk:conversion-inputs-not-installed',
                                       'value': canonical(sorted(CONVERSION_INPUTS))})
    sample = 'test-images/sample.jpg'
    components.append(dict(type='file', **{'bom-ref': 'sample-image'}, name=sample,
        hashes=[{'alg': 'SHA-256', 'content': entries[sample]['sha256']}],
        properties=[{'name': 'lwvk:artifact-role', 'value': 'user-provided evaluation sample; redistribution rights require maintainer confirmation'}]))
    owned.add(sample)
    missing = sorted(p for p in set(entries) - owned if not
        (p in ('LICENSE', 'NOTICE', 'models/onnx/catalog.json') or p.startswith('licenses/')))
    if missing:
        raise ValueError('component inventory omits locked source/model files: ' + ', '.join(missing))
    for platform in ('windows', 'linux'):
        sdk = lock['vulkan_sdk']
        components.append(dict(type='platform', **{'bom-ref': 'sdk-' + platform}, name='LunarG Vulkan SDK (' + platform + ')',
            version=sdk[platform + '_version'], scope='excluded', hashes=[{'alg': 'SHA-256', 'content': sdk[platform + '_sha256']}],
            externalReferences=[{'type': 'distribution', 'url': sdk['source']}],
            properties=[{'name': 'lwvk:artifact-role', 'value': 'build-only; not bundled; SDK has per-component licenses'}]))
    components.sort(key=lambda c: c['bom-ref'])
    root_component = dict(type='application', **{'bom-ref': 'lwvk'}, name='lw.PPOCR.Vulkan', version=version,
        licenses=[{'license': {'id': 'Apache-2.0'}}],
        properties=[{'name': 'lwvk:dependency-lock-sha256', 'value': digest(canonical(lock).encode())},
                    {'name': 'lwvk:coverage', 'value': 'reviewed sources/models/build SDK archives; not an exhaustive binary/OS/driver SBOM'}])
    return dict(**{'$schema': 'http://cyclonedx.org/schema/bom-1.6.schema.json'}, bomFormat='CycloneDX', specVersion='1.6',
        serialNumber='urn:uuid:' + str(uuid.uuid5(uuid.NAMESPACE_URL, 'https://github.com/lxw112190/lw.PPOCR.Vulkan/' + version + canonical(lock))),
        version=1, metadata={'component': root_component}, components=components,
        dependencies=[{'ref': 'lwvk', 'dependsOn': [c['bom-ref'] for c in components]}, {'ref': 'spdlog', 'dependsOn': ['fmt']}])


def validate_bom(root, bom, source=True):
    if bom.get('bomFormat') != 'CycloneDX' or bom.get('specVersion') != '1.6':
        raise ValueError('unsupported SBOM format/version')
    directory = root / ('third_party/cyclonedx' if source else 'metadata/cyclonedx')
    schemas = {name: json.loads((directory / name).read_text(encoding='utf-8')) for name in SCHEMAS}
    resources = [(base + name, Resource.from_contents(value)) for name, value in schemas.items()
                 for base in ('http://cyclonedx.org/schema/', 'https://cyclonedx.org/schema/')]
    # Explicit registry: missing refs fail instead of causing network requests.
    Draft7Validator(schemas[SCHEMAS[0]], registry=Registry().with_resources(resources),
                    format_checker=FormatChecker()).validate(bom)


def audit(root, source=True, write=False):
    lock = json.loads((root / 'dependencies.lock.json').read_text(encoding='utf-8'))
    entries = verify_files(root, lock, source)
    expected = make_bom(lock, (root / 'RELEASE_VERSION').read_text().strip())
    validate_bom(root, expected, source)
    path = root / 'SBOM.cdx.json'
    if write:
        path.write_text(json.dumps(expected, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    actual = json.loads(path.read_text(encoding='utf-8'))
    validate_bom(root, actual, source)
    if actual != expected:
        raise ValueError('SBOM differs from reviewed lock/version; regenerate explicitly with --write')
    return dict(passed=True, version=expected['metadata']['component']['version'], pinned_files=len(entries),
                components=len(expected['components']), scope='source/model inventory; not vulnerability clearance or complete binary provenance')


def validate_release_build(info, version, platform):
    expected_system = 'Windows' if platform == 'windows-x64' else 'Linux'
    if (info.get('metadata_version') != 1 or info.get('version') != version or
            info.get('system') != expected_system or info.get('pointer_bytes') != 8 or
            info.get('processor', '').lower() not in ('amd64', 'x86_64', 'x64') or
            info.get('configuration') != 'Release' or
            any(info.get(k) != 'OFF' for k in ('sanitizers', 'experimental_coop', 'experimental_rec_lanes', 'experimental_short_rec'))):
        raise ValueError('build metadata does not describe the default x64 Release artifact')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--root', type=Path, default=ROOT)
    p.add_argument('--package', action='store_true')
    p.add_argument('--write', action='store_true')
    p.add_argument('--report', type=Path)
    a = p.parse_args()
    if a.write and a.package:
        p.error('do not regenerate metadata in an installed package')
    result = audit(a.root.resolve(), not a.package, a.write)
    if a.report:
        a.report.parent.mkdir(parents=True, exist_ok=True)
        a.report.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
