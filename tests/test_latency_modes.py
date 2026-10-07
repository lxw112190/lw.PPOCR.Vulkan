"""Host gates for deterministic mixed-shape scheduling and pooled percentiles."""
import unittest
from benchmark_latency_modes import schedule, summarize, digest


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


if __name__ == '__main__':
    unittest.main()
