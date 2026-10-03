"""Read-only first-push checks for this project, even before its own git init.

This is a repository-content gate, not GPU/CI certification or a secret scanner.
"""
import argparse
import fnmatch
import json
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
SKIP_DIRS = {'.git','.cache','.ci','.vs','__pycache__','bin','obj','logs','dist'}
SKIP_FILES = ('*.pyc','*.pyo','*.log','*.exe','*.dll','*.lib','*.pdb','*.obj','*.user','*.suo')

def inventory():
    for directory, folders, names in os.walk(ROOT):
        base = Path(directory)
        folders[:] = [d for d in folders if d not in SKIP_DIRS and
            not (base == ROOT and d.startswith('build'))]
        for name in names:
            if not any(fnmatch.fnmatch(name,p) for p in SKIP_FILES):
                path = base/name
                if path.is_symlink():
                    raise RuntimeError('review symlink before first push: '+str(path.relative_to(ROOT)))
                yield path

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--report',type=Path)
    a = p.parse_args()
    required = ['README.md','README_EN.md','LICENSE','NOTICE','.gitignore','.gitattributes',
        '.clang-format','RELEASE_VERSION','CMakeLists.txt','dependencies.lock.json',
        '.github/workflows/build.yml','docs/DEVELOPMENT.md']
    for name in required:
        if not (ROOT/name).is_file():raise RuntimeError('missing repository file: '+name)
    workflow = (ROOT/'.github/workflows/build.yml').read_text(encoding='utf-8')
    assert 'branches: [main, master]' in workflow and 'contents: read' in workflow
    # Auxiliary quality workflows must not silently reference absent scripts.
    workflows = list((ROOT/'.github/workflows').glob('*.yml'))
    scripts = sorted(set(name for path in workflows
        for name in re.findall(r'\b(?:scripts|tests)/[\w./-]+\.(?:py|ps1|sh)',path.read_text(encoding='utf-8'))))
    for name in scripts:
        if not (ROOT/name).is_file():raise RuntimeError('workflow references missing script: '+name)
    for name in ('README.md','README_EN.md','docs/CSHARP-SHARE-PACKAGE.md'):
        text = (ROOT/name).read_text(encoding='utf-8')
        assert '264292622' in text and '758616458' not in text and 'C# 人工智能实践' not in text
    config = json.loads((ROOT/'http-service.json').read_text(encoding='utf-8'))
    if config.get('api_key'):raise RuntimeError('do not commit a real/default API Key')
    files = list(inventory())
    large = sorted((dict(path=f.relative_to(ROOT).as_posix(),bytes=f.stat().st_size) for f in files
        if f.stat().st_size > 50*1024**2),key=lambda x:x['bytes'],reverse=True)
    if any(x['bytes'] > 100*1024**2 for x in large):raise RuntimeError('file exceeds GitHub regular-Git limit')
    # Require explicit local-cache/build/byte-sensitive dictionary rules.
    ignore = (ROOT/'.gitignore').read_text()
    assert all(x in ignore for x in ('/build*/','/dist/','/.ci/','/.cache/','__pycache__/'))
    assert 'models/**/dictionary.txt -text' in (ROOT/'.gitattributes').read_text()
    subprocess.run([sys.executable,str(ROOT/'scripts/verify_assets.py')],cwd=ROOT,check=True)
    report = dict(passed=True,version=(ROOT/'RELEASE_VERSION').read_text().strip(),
        candidate_files=len(files),candidate_bytes=sum(f.stat().st_size for f in files),
        workflow_scripts_checked=scripts,files_over_50_mib=large,
        own_git_directory=(ROOT/'.git').exists(),online_actions_performed=False,
        limitations='Content/size/ignore checks only; inspect git staged paths and scan for secrets manually. No GPU/CI success claim.')
    if a.report:
        a.report.parent.mkdir(parents=True,exist_ok=True)
        a.report.write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(report,ensure_ascii=True,indent=2))

if __name__ == '__main__':main()
