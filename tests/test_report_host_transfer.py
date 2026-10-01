import copy
import importlib.util
from pathlib import Path
import unittest

path = Path(__file__).resolve().parents[1]/'scripts/report_host_transfer.py'
spec = importlib.util.spec_from_file_location('report_host_transfer', path)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class AuditTests(unittest.TestCase):
    def setUp(self):
        self.old = dict(status='passed', failures=[], unstable_content_or_boxes=[],
            backend='vulkan', model='tiny', images=100, method=dict(repeats=3),
            dataset_manifest_sha256='dataset', source_models_sha256={'rec':'model'},
            config=dict(det_limit_side=960), accuracy=dict(exact_lines=370),
            latency_ms=dict(mean=80, samples=300), results=[dict(file=str(i),sha256=str(i),
                size=[500,500], predictions=[dict(text='abc',score=.9)],
                accuracy=dict(exact=True)) for i in range(100)])
        self.new = copy.deepcopy(self.old)
        self.new['latency_ms']['mean'] = 50

    def test_identical_results_qualify(self):
        r = module.audit_pair(self.old,self.new)
        self.assertEqual(r['exact_prediction_objects'],100)
        self.assertEqual(r['mean_latency_reduction_percent'],37.5)

    def test_confidence_change_is_not_hidden_by_text_signature(self):
        self.new['results'][48]['predictions'][0]['score'] = .91
        with self.assertRaisesRegex(ValueError,'predictions'):
            module.audit_pair(self.old,self.new)

    def test_conditions_and_failures_are_rejected(self):
        for key,value in [('config',dict(det_limit_side=736)),('failures',['error']),
                          ('unstable_content_or_boxes',['image']),('images',99)]:
            with self.subTest(key=key):
                changed = copy.deepcopy(self.new)
                changed[key] = value
                with self.assertRaises(ValueError):
                    module.audit_pair(self.old,changed)


if __name__ == '__main__':
    unittest.main()
