"""Run all shader gates; diagnostic controls never forgive primary failures."""
import argparse
import json
import os
import re
from pathlib import Path
import subprocess
import sys
import time

PROBES = ('lwvk_det_gpu_preprocess_probe', 'lwvk_text_gpu_preprocess_probe',
          'lwvk_gpu_crop_probe')
PRIMARY_TIMEOUT_SECONDS = 420
CONTROL_TIMEOUT_SECONDS = 90


def diagnostic_env(source, *, no_layers=False, slow_stacks=False,
                   no_mesa_select=False):
    env = source.copy()
    # Slow unwinding on every Mesa/LLVM allocation makes shader compilation
    # extremely expensive. Reserve it for bounded enumeration-only controls.
    extra = ('detect_leaks=1:leak_check_at_exit=1:halt_on_error=1:'
             f'fast_unwind_on_malloc={0 if slow_stacks else 1}:'
             f'malloc_context_size={40 if slow_stacks else 30}:symbolize=1')
    env['ASAN_OPTIONS'] = env.get('ASAN_OPTIONS', '') + ':' + extra
    env['VK_LOADER_DISABLE_DYNAMIC_LIBRARY_UNLOADING'] = '0'
    if no_mesa_select or no_layers:
        # Jammy loader 1.3.204 predates VK_LOADER_LAYERS_DISABLE. The layer's
        # manifest-defined switch works there without disabling Khronos validation.
        env['NODEVICE_SELECT'] = '1'
    if no_layers:
        # VK_INSTANCE_LAYERS overrides the disable filter; remove it too.
        for key in ('VK_INSTANCE_LAYERS', 'VK_LOADER_LAYERS_ENABLE', 'VK_LOADER_LAYERS_ALLOW'):
            env.pop(key, None)
        env['VK_LOADER_LAYERS_DISABLE'] = '*'
    return env


def unloaded_frame_mappings(value):
    """Translate unknown PCs using pre-dlclose PT_LOAD ranges, not guessed ASLR bases."""
    modules = []
    for match in re.finditer(r'^LWVK_MODULE (0x[0-9a-f]+) (0x[0-9a-f]+) '
                             r'(0x[0-9a-f]+) (.+)$', value, re.MULTILINE):
        start, end, base = (int(match[i], 16) for i in (1, 2, 3))
        modules.append((start, end, base, match[4]))
    results = []
    for match in re.finditer(r'^\s*#\d+ (0x[0-9a-f]+)\s+\(<unknown module>\)',
                             value, re.MULTILINE):
        pc = int(match[1], 16)
        matches = {(path, pc - base) for start, end, base, path in modules
                   if start <= pc < end}
        # Multiple snapshots can reuse an address for different DSOs. Report
        # ambiguity rather than assigning a false library/function attribution.
        results.append(dict(pc=match[1], candidates=[
            dict(path=path, elf_address=hex(offset)) for path, offset in sorted(matches)]))
    return results


def run_case(command, output, label, env, timeout=PRIMARY_TIMEOUT_SECONDS):
    log = output / (label + '.log')
    timed_out = False
    started = time.monotonic()
    print(f'START: {label}, timeout={timeout}s, log={log}', flush=True)
    with log.open('w', encoding='utf-8') as stream:
        stream.write('COMMAND: ' + repr(command) + '\n')
        for key in ('ASAN_OPTIONS', 'ASAN_SYMBOLIZER_PATH', 'VK_ICD_FILENAMES',
                    'VK_INSTANCE_LAYERS', 'VK_LOADER_LAYERS_DISABLE',
                    'NODEVICE_SELECT', 'LWVK_DIAG_MODULE_MAPS',
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
    elapsed = round(time.monotonic() - started, 3)
    print(f'--- {label}: exit={code}, elapsed={elapsed}s, log={log} ---', flush=True)
    value = log.read_text(encoding='utf-8', errors='replace')
    print(value, flush=True)
    frames = unloaded_frame_mappings(value)
    if frames:
        mapping_file = output / (label + '-unloaded-frames.json')
        mapping_file.write_text(json.dumps(frames, indent=2) + '\n', encoding='utf-8')
        print('UNLOADED MODULE FRAMES: ' + json.dumps(frames), flush=True)
    return dict(label=label, command=command, exit_code=code, timed_out=timed_out,
                elapsed_seconds=elapsed, timeout_seconds=timeout)


def run_suite(build, output, device, source_env=None):
    output.mkdir(parents=True, exist_ok=True)
    env = diagnostic_env(os.environ if source_env is None else source_env)
    primary = []
    controls = []
    report = dict(passed=False, completed=False, primary=primary,
                  diagnostic_controls=controls,
                  max_child_seconds=len(PROBES) * PRIMARY_TIMEOUT_SECONDS +
                                    3 * CONTROL_TIMEOUT_SECONDS,
                  policy='Only ordinary-teardown primary runs decide this gate. '
                         'Controls cannot suppress, excuse or fix a primary failure.')

    def save_report():
        # Persist after each child, before any expensive diagnostic. Cancellation
        # must not erase completed gates or leave a misleading passing summary.
        (output / 'summary.json').write_text(json.dumps(report, indent=2) + '\n',
                                             encoding='utf-8')

    save_report()
    for name in PROBES:
        primary.append(run_case([str(build / name), str(device)], output, name, env))
        save_report()
    failed = [item for item in primary if item['exit_code'] != 0]
    if failed:
        # The reported 128-byte leak also occurs during enumeration. Do not
        # compile the same failing/timed-out shaders another six times.
        # Older loaders may ignore these diagnostic filters: inspect LD_DEBUG.
        for label, options in (
                ('control-enumeration', {}),
                ('control-enumeration-without-mesa-select', dict(no_mesa_select=True)),
                ('control-enumeration-without-layers', dict(no_layers=True))):
            control_env = diagnostic_env(env, slow_stacks=True, **options)
            control_env['LD_DEBUG'] = 'libs'
            control_env['VK_LOADER_DEBUG'] = 'layer'
            control_env['LWVK_DIAG_MODULE_MAPS'] = '1'
            controls.append(run_case([str(build / 'lw-ppocr-vulkan-probe')], output,
                                     label, control_env, timeout=CONTROL_TIMEOUT_SECONDS))
            save_report()
    report.update(passed=not failed, completed=True)
    save_report()
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
