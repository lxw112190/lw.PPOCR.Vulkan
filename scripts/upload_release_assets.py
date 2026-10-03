"""Upload verified tag packages; never replace assets or publish drafts."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile
from verify_archive import verify


def digest(path):
    value = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(chunk)
    return value.hexdigest()


def validate_assets(directory, tag):
    if not re.fullmatch(r'v\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?', tag):
        raise ValueError('Expected vMAJOR.MINOR.PATCH[-prerelease]')
    version = tag[1:]
    preview = '-preview' if '-' in version else ''
    packages = [(directory / f'lw.PPOCR.Vulkan-v{version}-{platform}-full-ocr{preview}.{suffix}', platform)
                for platform, suffix in [('windows-x64', 'zip'), ('linux-x64', 'tar.gz')]]
    expected = {p.name for path, _ in packages for p in (path, path.with_name(path.name + '.sha256'))}
    actual = {p.name for p in directory.iterdir()}
    if actual != expected or any(not p.is_file() or p.is_symlink() for p in directory.iterdir()):
        raise ValueError(f'Expected exactly two deployment archives and their checksums; got {sorted(actual)}')
    for path, platform in packages:
        manifest = verify(path)  # Entire archive, payload inventory and sidecar.
        if manifest['version'] != version or manifest['platform'] != platform:
            raise ValueError(f'Tag/platform mismatch in {path.name}')
        if manifest['contract_status'] != 'frozen':
            raise ValueError(f'Unfrozen deployment contract in {path.name}')
    return sorted(directory.iterdir())


def gh(*args, check=True):
    result = subprocess.run(['gh', *args], capture_output=True, encoding='utf-8',
                            errors='replace', timeout=300)
    if check and result.returncode:
        raise RuntimeError(result.stderr.strip() or 'GitHub CLI request failed')
    return result


def upload(directory, tag, repo):
    if not re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', repo):
        raise ValueError('Expected owner/repository')
    paths = validate_assets(directory, tag)  # No network/writes until validation passes.
    result = gh('api', f'repos/{repo}/releases/tags/{tag}', check=False)
    if result.returncode:
        if 'HTTP 404' not in result.stderr:
            raise RuntimeError(result.stderr.strip() or 'Could not query Release')
        release = None
    else:
        release = json.loads(result.stdout)
        if release.get('tag_name') != tag:
            raise ValueError('GitHub returned an unexpected Release tag')
    existing = {item['name']: item for item in release['assets']} if release else {}
    pending = []
    # Check ALL name conflicts before uploading anything. No --clobber or deletion.
    for path in paths:
        asset = existing.get(path.name)
        if not asset:
            pending.append(path)
            continue
        wanted = 'sha256:' + digest(path)
        actual = asset.get('digest')
        if not actual:
            # Older/manual assets may lack the server digest; compare real bytes.
            with tempfile.TemporaryDirectory() as temp:
                gh('release', 'download', tag, '--repo', repo, '--pattern', path.name, '--dir', temp)
                actual = 'sha256:' + digest(Path(temp) / path.name)
        if actual != wanted:
            raise ValueError(f'Existing asset differs: {path.name}; not replacing a released file. '
                             'Use a new version or review the conflict manually.')
        print(f'SKIP: identical existing asset {path.name}')
    if release is None:
        args = ['release', 'create', tag, '--repo', repo, '--verify-tag', '--draft',
                '--title', f'{tag} | lw.PPOCR.Vulkan', '--notes',
                'CI deployment packages / CI 部署包。请完成最终附件验收并补充发布说明后手动发布。']
        if '-' in tag:
            args.append('--prerelease')
        gh(*args)
    if pending:
        gh('release', 'upload', tag, *(str(p.resolve()) for p in pending), '--repo', repo)
        print(f'PASS: uploaded {len(pending)} missing assets to {repo} {tag}')
    else:
        print('PASS: all deployment assets already match; nothing modified')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', type=Path, required=True)
    parser.add_argument('--tag', required=True)
    parser.add_argument('--repo', required=True)
    parser.add_argument('--dry-run', action='store_true', help='Validate local files only; no network')
    args = parser.parse_args()
    if args.dry_run:
        print('PASS: ' + ', '.join(p.name for p in validate_assets(args.directory, args.tag)))
    else:
        upload(args.directory, args.tag, args.repo)


if __name__ == '__main__':
    main()
