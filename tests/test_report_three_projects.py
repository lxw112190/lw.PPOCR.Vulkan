"""Content agreement must not accidentally be presented as GT accuracy."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from report_three_projects import compare,memory


class Report(unittest.TestCase):
    def test_agreement_excludes_shared_misses(self):
        reports=[]
        for model in ('tiny','small','medium'):
            for backend in ('c','dml','vulkan'):
                texts={'c':['ok',None,'bad'],'dml':['ok',None,'bad'],'vulkan':['ok','ok','ok']}[backend]
                ds=[dict(expected='ok',predicted=t,exact=t=='ok',orientation_degrees=0) for t in texts]
                reports.append(dict(model=model,backend=backend,results=[dict(file='1.jpg',accuracy=dict(details=ds))]))
        report=compare(reports)
        cd=report['statistics'][0];cv=report['statistics'][1]
        self.assertEqual(cd['common_matched_lines'],2)
        self.assertEqual(cd['identical_text_lines'],2)
        self.assertEqual(cd['a_only_exact'],0)
        self.assertEqual(cd['b_only_exact'],0)
        self.assertEqual(cv['identical_text_lines'],1)
        self.assertEqual(cv['b_only_exact'],2)

    def test_missing_memory_not_zero(self):
        self.assertEqual(memory(None),'—')
        self.assertEqual(memory(0),'0.0')
        self.assertEqual(memory(1048576),'1.0')


if __name__=='__main__':unittest.main()
