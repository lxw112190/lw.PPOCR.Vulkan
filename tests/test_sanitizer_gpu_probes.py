"""Host-only diagnostic policy tests, not Linux/Vulkan qualification."""
import importlib.util
import json
import os
from pathlib import Path
import sys
import subprocess
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('gpu_probes', ROOT / 'scripts/sanitizer_gpu_probes.py')
probes = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probes)


class ProbeGateTests(unittest.TestCase):
    def test_env_retains_checks_and_does_not_mutate_input(self):
        source = dict(ASAN_OPTIONS='strict_string_checks=1', VK_INSTANCE_LAYERS='validation',
                      VK_LOADER_LAYERS_ENABLE='*', VK_LOADER_LAYERS_ALLOW='*')
        primary = probes.diagnostic_env(source)
        self.assertIn('detect_leaks=1', primary['ASAN_OPTIONS'])
        self.assertIn('leak_check_at_exit=1', primary['ASAN_OPTIONS'])
        self.assertIn('fast_unwind_on_malloc=1:malloc_context_size=30', primary['ASAN_OPTIONS'])
        self.assertNotIn('fast_unwind_on_malloc=0', primary['ASAN_OPTIONS'])
        self.assertEqual(primary['VK_LOADER_DISABLE_DYNAMIC_LIBRARY_UNLOADING'], '0')
        self.assertEqual(primary['VK_INSTANCE_LAYERS'], 'validation')
        mesa_control = probes.diagnostic_env(source, no_mesa_select=True)
        self.assertEqual(mesa_control['NODEVICE_SELECT'], '1')
        self.assertEqual(mesa_control['VK_INSTANCE_LAYERS'], 'validation')
        self.assertNotIn('NODEVICE_SELECT', source)
        control = probes.diagnostic_env(source, no_layers=True,
                                        slow_stacks=True)
        self.assertIn('fast_unwind_on_malloc=0:malloc_context_size=40', control['ASAN_OPTIONS'])
        self.assertEqual(control['VK_LOADER_DISABLE_DYNAMIC_LIBRARY_UNLOADING'], '0')
        self.assertEqual(control['NODEVICE_SELECT'], '1')
        self.assertEqual(control['VK_LOADER_LAYERS_DISABLE'], '*')
        for key in ('VK_INSTANCE_LAYERS', 'VK_LOADER_LAYERS_ENABLE', 'VK_LOADER_LAYERS_ALLOW'):
            self.assertNotIn(key, control)
        self.assertEqual(source['VK_INSTANCE_LAYERS'], 'validation')

    def suite(self, failure):
        with tempfile.TemporaryDirectory() as temp:
            folder = Path(temp)
            calls = []

            def fake(command, output, label, env, timeout=probes.PRIMARY_TIMEOUT_SECONDS):
                calls.append((label, env))
                code = 1 if failure and label == probes.PROBES[0] else 0
                return dict(label=label, command=command, exit_code=code, timed_out=False)

            with mock.patch.object(probes, 'run_case', side_effect=fake):
                code = probes.run_suite(folder, folder / 'logs', 0, {})
            report = json.loads((folder / 'logs/summary.json').read_text(encoding='utf-8'))
            return code, report, calls

    def test_failed_primary_remains_failed_even_when_controls_pass(self):
        code, report, calls = self.suite(True)
        self.assertEqual(code, 1)
        self.assertFalse(report['passed'])
        self.assertEqual([item[0] for item in calls[:3]], list(probes.PROBES))
        self.assertEqual(len(calls), 6)
        self.assertTrue(all(item['exit_code'] == 0 for item in report['diagnostic_controls']))
        self.assertTrue(report['completed'])
        self.assertTrue(all('fast_unwind_on_malloc=1' in env['ASAN_OPTIONS']
                            for _, env in calls[:3]))
        self.assertTrue(all('NODEVICE_SELECT' not in env for _, env in calls[:3]))
        self.assertEqual(calls[4][0], 'control-enumeration-without-mesa-select')
        self.assertEqual(calls[4][1]['NODEVICE_SELECT'], '1')
        self.assertTrue(all(env['LWVK_DIAG_MODULE_MAPS'] == '1'
                            for _, env in calls[3:]))

    def test_clean_primary_does_not_run_controls(self):
        code, report, calls = self.suite(False)
        self.assertEqual(code, 0)
        self.assertTrue(report['passed'])
        self.assertEqual(len(calls), 3)
        self.assertEqual(report['diagnostic_controls'], [])

    def test_all_timeouts_have_only_three_bounded_enumeration_controls(self):
        with tempfile.TemporaryDirectory() as temp:
            folder = Path(temp)
            calls = []

            def fake(command, output, label, env, timeout=probes.PRIMARY_TIMEOUT_SECONDS):
                calls.append((command, timeout))
                return dict(label=label, command=command, exit_code=124, timed_out=True)

            with mock.patch.object(probes, 'run_case', side_effect=fake):
                self.assertEqual(probes.run_suite(folder, folder / 'logs', 0, {}), 1)
            self.assertEqual(len(calls), 6)
            self.assertTrue(all(Path(command[0]).name == 'lw-ppocr-vulkan-probe'
                                for command, _ in calls[3:]))
            self.assertEqual([limit for _, limit in calls[3:]], [90, 90, 90])
            self.assertLessEqual(sum(limit for _, limit in calls), 26 * 60)
            report = json.loads((folder / 'logs/summary.json').read_text(encoding='utf-8'))
            self.assertFalse(report['passed'])

            self.assertEqual(report['max_child_seconds'], 1530)

    def test_unloaded_pc_uses_recorded_elf_load_bias(self):
        log = ('LWVK_MODULE 0x10000 0x10100 0x10000 /driver.so\n'
               'LWVK_MODULE 0x11000 0x12000 0x10000 /driver.so\n'
               '    #1 0x11042  (<unknown module>)\n'
               '    #2 0x99999  (<unknown module>)\n')
        frames = probes.unloaded_frame_mappings(log)
        self.assertEqual(frames[0]['candidates'],
                         [dict(path='/driver.so', elf_address='0x1042')])
        self.assertEqual(frames[1]['candidates'], [])

    def test_reused_module_ranges_are_reported_as_ambiguous(self):
        log = ('LWVK_MODULE 0x1000 0x2000 0x1000 /driver.so\n'
               'LWVK_MODULE 0x1000 0x2000 0x1000 /layer with space.so\n'
               '    #1 0x1428  (<unknown module>)\n')
        frames = probes.unloaded_frame_mappings(log)
        self.assertEqual(len(frames[0]['candidates']), 2)

    def test_missing_snapshot_does_not_guess_a_module(self):
        frames = probes.unloaded_frame_mappings('    #1 0x1234  (<unknown module>)\n')
        self.assertEqual(frames, [dict(pc='0x1234', candidates=[])])

    def test_partial_summary_survives_interruption_before_controls(self):
        with tempfile.TemporaryDirectory() as temp:
            folder = Path(temp)
            calls = []

            def fake(command, output, label, env, timeout=probes.PRIMARY_TIMEOUT_SECONDS):
                # The summary exists even before the first child finishes.
                report = json.loads((output / 'summary.json').read_text(encoding='utf-8'))
                self.assertFalse(report['passed'])
                self.assertFalse(report['completed'])
                self.assertEqual(len(report['primary']), len(calls))
                if len(calls) == 3:
                    raise KeyboardInterrupt
                calls.append(label)
                return dict(label=label, command=command, exit_code=1, timed_out=False)

            with mock.patch.object(probes, 'run_case', side_effect=fake):
                with self.assertRaises(KeyboardInterrupt):
                    probes.run_suite(folder, folder / 'logs', 0, {})
            report = json.loads((folder / 'logs/summary.json').read_text(encoding='utf-8'))
            self.assertEqual(len(report['primary']), 3)
            self.assertFalse(report['completed'])
            self.assertFalse(report['passed'])

    def test_real_child_failure_is_logged_not_swallowed(self):
        with tempfile.TemporaryDirectory() as temp:
            result = probes.run_case([sys.executable, '-c', 'print("fixture"); raise SystemExit(7)'],
                                     Path(temp), 'fixture', os.environ.copy())
            self.assertEqual(result['exit_code'], 7)
            self.assertGreaterEqual(result['elapsed_seconds'], 0)
            self.assertEqual(result['timeout_seconds'], 420)
            self.assertIn('fixture', (Path(temp) / 'fixture.log').read_text(encoding='utf-8'))

    def test_actual_child_snapshot_is_saved_without_changing_failure(self):
        with tempfile.TemporaryDirectory() as temp:
            child = ('print("LWVK_MODULE 0x1000 0x2000 0x1000 /driver.so"); '
                     'print("    #1 0x1428  (<unknown module>)"); raise SystemExit(7)')
            result = probes.run_case([sys.executable, '-c', child], Path(temp),
                                     'mapping', os.environ.copy())
            self.assertEqual(result['exit_code'], 7)
            frames = json.loads((Path(temp) / 'mapping-unloaded-frames.json').read_text(encoding='utf-8'))
            self.assertEqual(frames[0]['candidates'],
                             [dict(path='/driver.so', elf_address='0x428')])

    def test_launch_failure_is_logged(self):
        with tempfile.TemporaryDirectory() as temp:
            result = probes.run_case([str(Path(temp) / 'missing')], Path(temp), 'missing', {})
            self.assertEqual(result['exit_code'], 127)
            self.assertIn('could not start', (Path(temp) / 'missing.log').read_text(encoding='utf-8'))

    def test_timeout_is_a_failure_and_has_a_log(self):
        with tempfile.TemporaryDirectory() as temp:
            with mock.patch.object(probes.subprocess, 'run',
                                   side_effect=subprocess.TimeoutExpired(['fixture'], 1)):
                result = probes.run_case(['fixture'], Path(temp), 'timeout', {}, timeout=1)
            self.assertEqual(result['exit_code'], 124)
            self.assertTrue(result['timed_out'])
            self.assertIn('exceeded', (Path(temp) / 'timeout.log').read_text(encoding='utf-8'))

    def test_workflow_keeps_leak_gate_symbolizer_and_failure_artifact(self):
        workflow = (ROOT / '.github/workflows/sanitizers.yml').read_text(encoding='utf-8')
        for required in ('detect_leaks=1:halt_on_error=1', 'llvm-14',
                         'ASAN_SYMBOLIZER_PATH: /usr/bin/llvm-symbolizer-14',
                         'scripts/sanitizer_gpu_probes.py', 'if: always()',
                         'timeout-minutes: 30', 'actions/checkout@v5',
                         'actions/setup-python@v6', 'actions/cache@v5',
                         'actions/upload-artifact@v6',
                         'build/reports/sanitizer/**'):
            self.assertIn(required, workflow)
        for forbidden in ('detect_leaks=0', 'continue-on-error:', 'LSAN_OPTIONS:',
                          'VK_LOADER_DISABLE_DYNAMIC_LIBRARY_UNLOADING:'):
            self.assertNotIn(forbidden, workflow)


if __name__ == '__main__':
    unittest.main()
