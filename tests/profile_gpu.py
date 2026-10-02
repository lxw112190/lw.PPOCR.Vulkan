"""Engineering-only operator timestamps, never an end-to-end performance claim.

Each case runs in a new process (static CRT environment is read at DLL load).
Control processes with profiling disabled must return identical result digests.
Only shapes, timings, hashes and operator metadata are persisted, not OCR text.
"""
import argparse
import ctypes as C
import hashlib
import json
import math
import os
import statistics
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PREFIX = "LWVK_GPU_PROFILE "
CONTROL_PREFIX = "LWVK_PROFILE_CONTROL "
CASES = [("det",736,960), ("det",960,960), ("rec",48,320),
         ("rec",48,960), ("cls",80,160)]


def parse_child_output(stdout, stderr):
    # The validation layer may log to stdout (not only stderr). Parse our
    # explicitly tagged control instead of assuming stdout is a JSON document.
    diagnostics = stdout + '\n' + stderr
    if any(marker in diagnostics for marker in ('Validation Error', 'VUID-', 'SYNC-HAZARD')):
        raise AssertionError('Vulkan validation error:\n' + diagnostics[-8000:])
    controls = [json.loads(line[len(CONTROL_PREFIX):]) for line in stdout.splitlines()
                if line.startswith(CONTROL_PREFIX)]
    if len(controls) != 1:
        raise AssertionError('expected exactly one tagged profile control')
    records = [json.loads(line[len(PREFIX):]) for line in diagnostics.splitlines() if line.startswith(PREFIX)]
    return controls[0], records


def child(a):
    import numpy as np
    sys.path.insert(0,str(ROOT / "examples/python"))
    from lwvk import load, check, Network, DeviceInfo
    lib=load(a.library)
    info=DeviceInfo(struct_size=C.sizeof(DeviceInfo))
    check(lib,lib.lwvk_device_get(a.device,C.byref(info)))
    name=info.name.decode("utf-8")
    if a.require_device and a.require_device not in name:
        raise AssertionError("wrong GPU hardware")
    task,height,width=a.case.split(":")
    height,width=int(height),int(width)
    model=ROOT / "models/onnx" / ("ppocrv6-"+a.variants[0]) / (task+".onnx")
    tensor=np.random.default_rng(20261001).uniform(-1,1,(1,3,height,width)).astype(np.float32)
    digests=[]
    with Network(lib,model,a.device) as network:
        for _ in range(4):
            output,_=network.run(tensor)
            assert np.all(np.isfinite(output)),"non-finite probabilities"
            digest=hashlib.sha256(output.tobytes()).hexdigest()
            if task=="rec":
                text,score,_=network.recognize(tensor)
                assert math.isfinite(score) and 0<=score<=1
                digest+=hashlib.sha256(text.encode()+np.float32(score).tobytes()).hexdigest()
            digests.append(digest)
    assert len(set(digests))==1,"repeated calls changed results"
    print(CONTROL_PREFIX+json.dumps(dict(device=name,version=lib.lwvk_version().decode(),
        model_sha256=hashlib.sha256(model.read_bytes()).hexdigest(),result_digest=digests[0])))


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--library",type=Path,required=True)
    p.add_argument("--device",type=int,default=1)
    p.add_argument("--require-device",default="RTX 4060")
    p.add_argument("--variants",choices=("tiny","small","medium"),nargs="+",default=["medium"])
    p.add_argument("--coop",action="store_true",help="explicit experimental build + runtime opt-in")
    p.add_argument("--coop-det-only",action="store_true",help="use mixed-precision dispatch only in DET; requires --coop")
    p.add_argument("--tile64",action="store_true",help="explicit experimental FP32 M64 pointwise tile; default off")
    p.add_argument("--validation",action="store_true",help="request Khronos validation layer; errors fail the diagnostic")
    p.add_argument("--report",type=Path)
    p.add_argument("--case",help=argparse.SUPPRESS)
    a=p.parse_args()
    if a.coop_det_only and not a.coop:
        p.error("--coop-det-only requires --coop")
    for stream in (sys.stdout,sys.stderr):
        if hasattr(stream,"reconfigure"):
            stream.reconfigure(encoding="utf-8",errors="backslashreplace")
    if a.case:
        child(a)
        return 0
    if not a.report:
        p.error("--report is required")
    reports=[]
    for variant in a.variants:
        for task,height,width in CASES:
            controls=[]
            records=[]
            for enabled in (False,True):
                env=os.environ.copy()
                env["LWVK_GPU_PROFILE"]="1" if enabled else "0"
                env["LWVK_EXPERIMENTAL_COOP"]="1" if a.coop else "0"
                env["LWVK_EXPERIMENTAL_COOP_DET_ONLY"]="1" if a.coop_det_only else "0"
                env["LWVK_EXPERIMENTAL_TILE64"]="1" if a.tile64 else "0"
                if a.validation:
                    env["VK_INSTANCE_LAYERS"]="VK_LAYER_KHRONOS_validation"
                args=[sys.executable,str(Path(__file__).resolve()),"--library",str(a.library.resolve()),
                    "--device",str(a.device),"--require-device",a.require_device,
                    "--variants",variant,"--case",f"{task}:{height}:{width}"]
                result=subprocess.run(args,env=env,capture_output=True,text=True,encoding="utf-8",errors="replace",timeout=180)
                if result.returncode:
                    raise RuntimeError(f"profile child failed ({variant}/{task}/{width}):\n{result.stderr[-8000:]}")
                control, current = parse_child_output(result.stdout, result.stderr)
                controls.append(control)
                if not enabled:
                    assert not current,"profiling disabled but timestamps emitted"
                else:
                    records=current
            assert controls[0]==controls[1],"instrumentation changed output or device"
            expected=6 if task=="rec" else 3
            assert len(records)==expected,"expected three bounded samples per normal/CTC command"
            summaries=[]
            for ctc in ([False,True] if task=="rec" else [False]):
                samples=[r for r in records if r["ctc"]==ctc]
                assert [r["sample"] for r in samples]==[1,2,3]
                reference=samples[0]["dispatches"]
                operators=[]
                for index,dispatch in enumerate(reference):
                    values=[]
                    for record in samples:
                        assert record["input_hw"]==[height,width] and record["task"]==task
                        assert 1<=record["timestamp_valid_bits"]<=64
                        assert record["timestamp_period_ns"]>0
                        item=record["dispatches"][index]
                        assert item["index"]==index and item["shader"]==dispatch["shader"]
                        assert item["groups"]==dispatch["groups"] and item["push"]==dispatch["push"]
                        assert math.isfinite(item["gpu_ms"]) and 0<=item["gpu_ms"]<30000
                        values.append(item["gpu_ms"])
                    # First execution is warm-up; retain all raw measurements too.
                    operators.append(dict(index=index,shader=dispatch["shader"],groups=dispatch["groups"],
                        push=dispatch["push"],samples_ms=values,median_ms=statistics.median(values[1:])))
                by_shader={}
                for op in operators:
                    by_shader[op["shader"]]=by_shader.get(op["shader"],0)+op["median_ms"]
                summaries.append(dict(ctc=ctc,dispatch_count=len(operators),
                    cooperative_matrix=samples[0]["cooperative_matrix"],
                    dispatch_sum_ms=sum(o["median_ms"] for o in operators),
                    by_shader_ms=by_shader,top_operators=sorted(operators,key=lambda o:o["median_ms"],reverse=True)[:15],
                    operators=operators))
            case=dict(model=variant,task=task,input_hw=[height,width],**controls[0],profiles=summaries)
            reports.append(case)
            if a.coop_det_only and task!="det":
                assert all("conv_coop_gemm" not in s["by_shader_ms"] for s in summaries),"mixed-precision dispatch escaped DET"
            print(json.dumps(dict(model=variant,task=task,input_hw=[height,width],
                hotspots=[s["by_shader_ms"] for s in summaries]),ensure_ascii=False))
    if a.coop:
        assert any(s["cooperative_matrix"] for r in reports for s in r["profiles"]),"experiment not enabled by build"
        assert any("conv_coop_gemm" in s["by_shader_ms"] for r in reports for s in r["profiles"]),"no cooperative dispatch tested"
    data=dict(status="passed_diagnostic_not_benchmark",schema_version=1,
        library_sha256=hashlib.sha256(a.library.read_bytes()).hexdigest(),device_index=a.device,
        experimental_coop=a.coop,experimental_coop_det_only=a.coop_det_only,experimental_tile64=a.tile64,validation_requested=a.validation,cases=reports,
        method="Deterministic normalized random tensors; four runs per case and command, first three timestamped/logged. First sample excluded from medians. Profiling-off control output digests identical. BOTTOM_OF_PIPE timestamps serialize and perturb execution; sums exclude host work, transfers, inter-dispatch gaps. Not a quality test or end-to-end speed claim.")
    a.report.parent.mkdir(parents=True,exist_ok=True)
    a.report.write_text(json.dumps(data,ensure_ascii=False,indent=2)+"\n",encoding="utf-8")
    return 0


if __name__=="__main__":
    raise SystemExit(main())
