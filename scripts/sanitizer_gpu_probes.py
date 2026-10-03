"""Run all shader gates; diagnostic controls never forgive primary failures."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

PROBES = ('lwvk_det_gpu_preprocess_probe', 'lwvk_text_gpu_preprocess_probe',
          'lwvk_gpu_crop_probe')


def diagnostic_env(source, *, keep_modules=False, no_layers=False):
    env = source.copy()
    # Slow unwinding can traverse third-party builds without frame pointers.
    extra = ('detect_leaks=1:leak_check_at_exit=1:halt_on_error=1:'
             'fast_unwind_on_malloc=0:malloc_context_size=40:symbolize=1')
    env['ASAN_OPTIONS'] = env.get('ASAN_OPTIONS', '') + ':' + extra
    env['VK_LOADER_DISABLE_DYNAMIC_LIBRARY_UNLOADING'] = '1' if keep_modules else '0'
    if no_layers:
        # VK_INSTANCE_LAYERS overrides the disable filter; remove it too.
        for key in ('VK_INSTANCE_LAYERS', 'VK_LOADER_LAYERS_ENABLE', 'VK_LOADER_LAYERS_ALLOW'):
            env.pop(key, None)
        env['VK_LOADER_LAYERS_DISABLE'] = '*'
    return env


def run_case(command, output, label, env, timeout=420):
    log = output / (label + '.log')
    timed_out = False
    with log.open('w', encoding='utf-8') as stream:
        stream.write('COMMAND: ' + repr(command) + '\n')
        for key in ('ASAN_OPTIONS', 'ASAN_SYMBOLIZER_PATH', 'VK_ICD_FILENAMES',
                    'VK_INSTANCE_LAYERS', 'VK_LOADER_LAYERS_DISABLE',
                    'VK_LOADER_DISABLE_DYNAMIC_LIBRARY_UNLOADING'):
            stream.write(f'{key}={env.get(key, "<unset>")}\n')
        stream.flush()
        try:
            process = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT,
                                     env=env, timeout=timeout, check=False)
            code = process.returncode
        except subprocess.TimeoutExpired:
            # subprocess.run kills and waits for this native probe on timeout.
            code, timed_out = 124, True
            stream.write(f'ERROR: probe exceeded {timeout} seconds\n')
        except OSError as error:
            code = 127
            stream.write(f'ERROR: could not start probe: {error}\n')
    print(f'--- {label}: exit={code}, log={log} ---', flush=True)
    print(log.read_text(encoding='utf-8', errors='replace'), flush=True)
    return dict(label=label, command=command, exit_code=code, timed_out=timed_out)


def run_suite(build, output, device, source_env=None):
    output.mkdir(parents=True, exist_ok=True)
    env = diagnostic_env(os.environ if source_env is None else source_env)
    primary = []
    for name in PROBES:
        primary.append(run_case([str(build / name), str(device)], output, name, env))
    failed = [item for item in primary if item['exit_code'] != 0]
    controls = []
    if failed:
        # Enumeration only: no OCR network, buffers or shaders.
        controls.append(run_case([str(build / 'lw-ppocr-vulkan-probe')], output,
                                 'control-enumeration', env))
        for item in failed:
            # Khronos loader >= 1.3.259 supports this stack-diagnosis option.
            # Keeping DSOs loaded changes LSan global roots: diagnostic ONLY.
            retained = diagnostic_env(env, keep_modules=True)
            retained['LD_DEBUG'] = 'libs'
            controls.append(run_case(item['command'], output,
                                     item['label'] + '-retained-modules', retained))
            without_layers = diagnostic_env(env, no_layers=True)
            without_layers['LD_DEBUG'] = 'libs'
            controls.append(run_case(item['command'], output,
                                     item['label'] + '-without-layers', without_layers))
    report = dict(passed=not failed, primary=primary, diagnostic_controls=controls,
                  policy='Only ordinary-teardown primary runs decide this gate. '
                         'Controls cannot suppress, excuse or fix a primary failure.')
    (output / 'summary.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    if failed:
        print('FAIL: ordinary-teardown sanitizer probe(s) failed; diagnostic controls '
              'do not override this result. Inspect shader-probes/*.log.', flush=True)
    return 1 if failed else 0


def main():
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, 'reconfigure'):
            stream.reconfigure(encoding='utf-8', errors='backslashreplace')
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--device', type=int, default=0)
    args = parser.parse_args()
    return run_suite(args.build.resolve(), args.output.resolve(), args.device)


if __name__ == '__main__':
    raise SystemExit(main())
