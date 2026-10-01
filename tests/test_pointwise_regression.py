"""Strict old/new FP32 probability and CTC comparison for pointwise optimization.

Same hardware, seeded normalized tensors, no oracle framework, no timing claim.
Use independent ORT reference tests separately. No private images/text saved.
"""
import argparse
import hashlib
import json
from pathlib import Path
import sys
import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'examples/python'))
from lwvk import load, Network


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--before', type=Path, required=True)
    p.add_argument('--after', type=Path, required=True)
    p.add_argument('--device', type=int, default=1)
    p.add_argument('--models', nargs='+', choices=['tiny', 'small', 'medium'], default=['tiny', 'small', 'medium'])
    p.add_argument('--report', type=Path, required=True)
    a = p.parse_args()
    libraries = [load(a.before), load(a.after)]
    rows = []
    cases = {'det': [(64,64), (96,160), (320,320)], 'cls': [(80,160)],
             'rec': [(48,w) for w in (32,40,96,320,960)]}
    for model in a.models:
        for task, shapes in cases.items():
            nets = [Network(lib, ROOT/'models/onnx'/('ppocrv6-'+model)/(task+'.onnx'), a.device) for lib in libraries]
            try:
                for height, width in shapes:
                    value = np.random.default_rng(height*1000+width).uniform(-1,1,(1,3,height,width)).astype(np.float32)
                    old, _ = nets[0].run(value)
                    for _ in range(3):
                        new, _ = nets[1].run(value)
                        assert np.all(np.isfinite(new))
                        assert np.array_equal(old.view(np.uint32), new.view(np.uint32)), (model,task,height,width,float(np.max(np.abs(old-new))))
                    if task=='rec':
                        assert nets[0].recognize(value)[:2] == nets[1].recognize(value)[:2]
                    rows.append(dict(model=model, task=task, shape=[height,width], exact_fp32_bits=True,
                                     output_sha256=hashlib.sha256(old.tobytes()).hexdigest()))
                print(f'PASS: {model}/{task} exact FP32 bits', flush=True)
            finally:
                for net in nets: net.close()
    result = dict(passed=True, device=a.device, cases=rows,
        before_sha256=hashlib.sha256(a.before.read_bytes()).hexdigest(),
        after_sha256=hashlib.sha256(a.after.read_bytes()).hexdigest(),
        limitations='Same-device regression, not independent accuracy, benchmark or leak proof.')
    a.report.parent.mkdir(parents=True,exist_ok=True)
    a.report.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')


if __name__=='__main__': main()
