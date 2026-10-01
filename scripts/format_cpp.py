"""Format/check first-party C/C++ only; never rewrite third_party snapshots."""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--clang-format', type=Path)
    modes = p.add_mutually_exclusive_group()
    modes.add_argument('--write', action='store_true')
    modes.add_argument('--check', action='store_true', help='default mode')
    a = p.parse_args()
    executable = a.clang_format or shutil.which('clang-format')
    if not executable and os.name == 'nt':
        vs = Path(os.environ.get('ProgramFiles', 'C:/Program Files'))/'Microsoft Visual Studio/2022'
        executable = next(vs.glob('*/VC/Tools/Llvm/x64/bin/clang-format.exe'), None)
    if not executable:
        p.error('clang-format 17 is required; specify --clang-format PATH')
    version = subprocess.check_output([str(executable), '--version'], text=True)
    if not re.search(r'version 17\.', version):
        p.error('use clang-format 17 for reproducible formatting: '+version.strip())
    files = sorted(f for folder in ('src','apps','include','tests')
        for f in (ROOT/folder).rglob('*') if f.suffix in ('.c','.cpp','.h','.hpp'))
    if not files:
        raise RuntimeError('no first-party C/C++ files found')
    for path in files:
        if a.write:
            # clang-format 17 can need a second pass after expanding dense lambdas.
            for _ in range(2):
                subprocess.run([str(executable), '--style=file', '-i', str(path)], check=True, cwd=ROOT)
        subprocess.run([str(executable), '--style=file', '--dry-run', '--Werror', str(path)], check=True, cwd=ROOT)
    print(('Formatted' if a.write else 'PASS: formatted')+f' {len(files)} first-party files; third_party untouched')

if __name__ == '__main__':main()
