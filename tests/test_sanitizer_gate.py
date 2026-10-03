"""Prove instrumentation is active on every compiled TU, and faults fail CI."""
import argparse
import json
import os
from pathlib import Path
import subprocess

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--tripwire', type=Path, required=True)
    p.add_argument('--compile-commands', type=Path, required=True)
    a = p.parse_args()
    commands = json.loads(a.compile_commands.read_text())
    assert commands, 'No compile_commands entries'
    required = ('-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer')
    missing = []
    for item in commands:
        command = item.get('command') or ' '.join(item['arguments'])
        if not all(flag in command for flag in required):
            missing.append(item['file'])
    assert not missing, f'Uninstrumented C/C++ sources: {missing}'
    assert 'detect_leaks=1' in os.environ.get('ASAN_OPTIONS', ''), 'Explicit leak checking required'
    for mode, marker in [('asan', 'heap-buffer-overflow'),
                         ('ubsan', 'signed integer overflow'),
                         ('lsan', 'LeakSanitizer')]:
        proc = subprocess.run([str(a.tripwire.resolve()), mode], capture_output=True,
                              encoding='utf-8', errors='replace', timeout=30)
        output = proc.stdout + proc.stderr
        assert proc.returncode != 0 and marker in output, (mode, proc.returncode, output)
        print(f'PASS: intentional {mode} fault detected (exit {proc.returncode})')
    print(f'PASS: all {len(commands)} compilation units instrumented')

if __name__ == '__main__':
    main()
