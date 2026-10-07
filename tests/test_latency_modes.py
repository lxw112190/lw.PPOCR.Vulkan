"""Host gates for deterministic mixed-shape scheduling and pooled percentiles."""
import unittest
from unittest.mock import patch
from types import SimpleNamespace
from benchmark_latency_modes import schedule, summarize, digest, system_memory_snapshot


class LatencyModes(unittest.TestCase):
    def test_reverse_cycles(self):
        self.assertEqual(list(schedule(12, 5)), [0, 1, 2, 3, 4, 4, 3, 2, 1, 0, 0, 1])

    def test_empty_phase(self):
        self.assertEqual(summarize([]), {'calls': 0})

    def test_percentile_uses_all_samples(self):
        rows = [dict(wall_ms=v, timing={'total_ms': v - 1}) for v in (2, 3, 4, 101)]
        data = summarize(rows)
        self.assertEqual(data['calls'], 4)
        self.assertEqual(data['latency_ms']['wall_ms']['median'], 3.5)
        self.assertAlmostEqual(data['latency_ms']['wall_ms']['p95'], 86.45)

    def test_digest_all_fields(self):
        a = {'text': 'test', 'score': .9, 'x1': 1}
        self.assertEqual(digest([a]), digest([dict(reversed(list(a.items())))]))
        self.assertNotEqual(digest([a]), digest([dict(a, score=.8)]))

    def test_system_memory_observation(self):
        with patch('benchmark_latency_modes.psutil.virtual_memory', return_value=SimpleNamespace(total=16, available=2, percent=87.5)):
            value = system_memory_snapshot()
        self.assertEqual(value['system_total_bytes'], 16)
        self.assertEqual(value['system_available_bytes'], 2)
        self.assertEqual(value['system_memory_percent'], 87.5)
        self.assertIsNone(value['system_memory_error'])

    def test_system_memory_failure_is_unknown(self):
        with patch('benchmark_latency_modes.psutil.virtual_memory', side_effect=RuntimeError('unavailable')):
            value = system_memory_snapshot()
        self.assertIsNone(value['system_available_bytes'])
        self.assertIsNone(value['system_memory_percent'])
        self.assertEqual(value['system_memory_error'], 'unavailable')


if __name__ == '__main__':
    unittest.main()
