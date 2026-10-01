"""Host tests for corpus scoring; no inference or GPU needed."""
import ctypes
import sys
import unittest
from pathlib import Path

import benchmark_three_projects as bench
from benchmark_projects.adapters import OcrOptions,OcrInfo,OcrLine,OcrResult,Error

sys.path.insert(0,str(bench.ROOT.parent/'lw.PPOCR.C/tools'))
import evaluate_ocr_dataset as evaluator


def item(text,offset=0):
    return dict(text=text,x1=10+offset,y1=10,x2=90+offset,y2=10,
        x3=90+offset,y3=30,x4=10+offset,y4=30)


class Scoring(unittest.TestCase):
    record=dict(width=300,height=100,lines=[dict(text='abc',bbox=[10,10,90,30])])

    def score(self,items):return bench.evaluate(evaluator,self.record,items)

    def test_perfect(self):
        r=self.score([item('abc')]);self.assertEqual(r['exact_lines'],1)
        self.assertEqual(r['end_to_end_edits'],0)

    def test_missing_is_deletion(self):
        r=self.score([]);self.assertEqual(r['matched_lines'],0)
        self.assertEqual(r['end_to_end_edits'],3)
        s=bench.summarize([dict(accuracy=r)]);self.assertEqual(s['end_to_end_character_accuracy'],0)

    def test_false_positive_is_insertion(self):
        r=self.score([item('abc'),item('extra',150)])
        self.assertEqual(r['extra_characters'],5);self.assertEqual(r['end_to_end_edits'],5)

    def test_wrong_text(self):
        r=self.score([item('axc')]);self.assertEqual(r['end_to_end_edits'],1)
        self.assertEqual(r['exact_lines'],0)

    def test_duplicate_not_double_match(self):
        r=self.score([item('abc'),item('abc')]);self.assertEqual(r['matched_lines'],1)
        self.assertEqual(r['extra_characters'],3)

    def test_whitespace_case_not_normalized_away(self):
        self.assertEqual(self.score([item('ABC')])['end_to_end_edits'],3)
        self.assertEqual(self.score([item('abc ')])['end_to_end_edits'],1)

    def test_nonfinite_rejected(self):
        p=item('abc');p['x1']=float('nan')
        with self.assertRaises(ValueError):self.score([p])

    def test_nfc_equivalent(self):
        record=dict(width=300,height=100,lines=[dict(text='é',bbox=[10,10,90,30])])
        r=bench.evaluate(evaluator,record,[item('e\u0301')])
        self.assertEqual(r['exact_lines'],1)
        self.assertEqual(r['gt_characters'],1)

    def test_abi_layout(self):
        for t,size in ((OcrOptions,176),(OcrInfo,40),(OcrLine,80),(OcrResult,40),(Error,264)):
            self.assertEqual(ctypes.sizeof(t),size)


if __name__=='__main__':unittest.main()
