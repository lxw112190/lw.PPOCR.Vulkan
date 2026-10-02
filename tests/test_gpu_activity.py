"""Only counter aggregation semantics; no hardware compatibility claim."""
import unittest
from benchmark_projects.gpu_activity import activity_summary

class ActivitySummary(unittest.TestCase):
    def test_missing_not_zero(self):
        result=activity_summary([dict(process_busiest_engine_percent=None)])
        self.assertIsNone(result['process_busiest_engine_percent'])
        self.assertIsNone(result['board_gpu_percent'])

    def test_independent_metrics(self):
        result=activity_summary([
            dict(process_busiest_engine_percent=80,board_gpu_percent=90,board_memory_utilization_percent=30,
                process_engine_percent={'pid_1_engtype_Compute_0':80,'pid_1_engtype_Copy':40}),
            dict(process_busiest_engine_percent=20,board_gpu_percent=60,board_memory_utilization_percent=10)])
        self.assertEqual(result['process_busiest_engine_percent'],dict(mean=50,peak=80,samples=2))
        self.assertEqual(result['board_gpu_percent']['mean'],75)
        self.assertEqual(result['board_memory_utilization_percent']['mean'],20)
        self.assertEqual(result['per_engine_type'],dict(Compute_0=80,Copy=40))

if __name__=='__main__':unittest.main()
