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
        self.assertEqual(primary['VK_LOADER_DISABLE_DYNAMIC_LIBRARY_UNLOADING'], '0')
        self.assertEqual(primary['VK_INSTANCE_LAYERS'], 'validation')
        control = probes.diagnostic_env(source, keep_modules=True, no_layers=True)
        self.assertEqual(control['VK_LOADER_DISABLE_DYNAMIC_LIBRARY_UNLOADING'], '1')
        self.assertEqual(control['VK_LOADER_LAYERS_DISABLE'], '*')
        for key in ('VK_INSTANCE_LAYERS', 'VK_LOADER_LAYERS_ENABLE', 'VK_LOADER_LAYERS_ALLOW'):
            self.assertNotIn(key, control)
        self.assertEqual(source['VK_INSTANCE_LAYERS'], 'validation')

    def suite(self, failure):
        with tempfile.TemporaryDirectory() as temp:
            folder = Path(temp)
            calls = []

            def fake(command, output, label, env):
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

    def test_clean_primary_does_not_run_controls(self):
        code, report, calls = self.suite(False)
        self.assertEqual(code, 0)
        self.assertTrue(report['passed'])
        self.assertEqual(len(calls), 3)
        self.assertEqual(report['diagnostic_controls'], [])

    def test_real_child_failure_is_logged_not_swallowed(self):
        with tempfile.TemporaryDirectory() as temp:
            result = probes.run_case([sys.executable, '-c', 'print("fixture"); raise SystemExit(7)'],
                                     Path(temp), 'fixture', os.environ.copy())
            self.assertEqual(result['exit_code'], 7)
            self.assertIn('fixture', (Path(temp) / 'fixture.log').read_text(encoding='utf-8'))

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
                         'build/reports/sanitizer/**'):
            self.assertIn(required, workflow)
        for forbidden in ('detect_leaks=0', 'continue-on-error:', 'LSAN_OPTIONS:',
                          'VK_LOADER_DISABLE_DYNAMIC_LIBRARY_UNLOADING:'):
            self.assertNotIn(forbidden, workflow)


if __name__ == '__main__':
    unittest.main()
