"""Guard negative-result reports against selective pooling and false equality."""
import copy
from pathlib import Path
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from report_rec_lanes import summarize


def matrix():
    rows=[]
    for model in ('tiny','small','medium'):
        for mode in ('baseline','lanes1','lanes2','lanes4'):
            rows.append(dict(model=model,mode=mode,library_sha256=mode,source_models_sha256={},
                dataset_manifest_sha256='dataset',config={},initialization_ms=0,latency_ms={},timing_ms={},
                rec_lanes_requested=1,accuracy={},ram_after_each_repeat=[],status='passed',
                exact_items_equal_to_baseline=True,different_image_files=[],failures=[],unstable_content_or_boxes=[],
                memory={},results=[dict(file='image.jpg',predictions=[dict(text=model,score=.9)],samples_ms=[2.,4.])]))
    return dict(passed=True,comparisons=rows,repeats=2,reverse_order=False,images=1,hardware={})


class ReportTests(unittest.TestCase):
    def test_pool_samples_not_percentiles(self):
        result=summarize([matrix(),matrix()])
        row=result['aggregates'][0]
        self.assertEqual(row['samples'],4)
        self.assertEqual(row['mean_ms'],3.)
        self.assertIsNone(row['gpu_dedicated_peak_bytes'])
        self.assertEqual(result['measured_calls'],48)

    def test_incomplete_or_failed_matrix_rejected(self):
        value=matrix();value['passed']=False
        with self.assertRaises(ValueError):summarize([value])
        value=matrix();value['comparisons'].pop()
        with self.assertRaises(ValueError):summarize([value])

    def test_prediction_change_rejected(self):
        first=matrix();second=copy.deepcopy(first)
        second['comparisons'][0]['results'][0]['predictions'][0]['score']=.8
        with self.assertRaises(ValueError):summarize([first,second])

    def test_binary_change_not_pooled(self):
        first=matrix();second=copy.deepcopy(first)
        second['comparisons'][0]['library_sha256']='different'
        with self.assertRaises(ValueError):summarize([first,second])


if __name__=='__main__':unittest.main()
