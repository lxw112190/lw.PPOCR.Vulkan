"""Application-owned WinForms smoke: renders layout and drives real canvas handlers.
Host mode never claims GPU inference. Physical mode tests full OCR and selected ROI.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
for stream in (sys.stdout, sys.stderr):
    if hasattr(stream, "reconfigure"):
        stream.reconfigure(encoding="utf-8", errors="backslashreplace")
p = argparse.ArgumentParser(description=__doc__)
p.add_argument("--package", type=Path, required=True)
p.add_argument("--output", type=Path, required=True)
p.add_argument("--host-only", action="store_true")
p.add_argument("--device", type=int, default=0)
p.add_argument("--repeat", type=int, default=1)
p.add_argument("--gpu-det-preprocess", action="store_true", help="explicitly opt child process into the DET experiment")
a = p.parse_args()
package, output = a.package.resolve(), a.output.resolve()
output.mkdir(parents=True, exist_ok=True)
prefix = "host" if a.host_only else f"device{a.device}"
report, screenshot = output/f"winforms-{prefix}.json", output/f"winforms-{prefix}.png"
# Do not let stale results hide a missing output from this run.
for previous in (report, screenshot):
    if previous.is_file():
        previous.unlink()
exe = package/"lw.PPOCR.Vulkan.WinFormsDemo.exe"
args = [str(exe), "--smoke-host" if a.host_only else "--smoke", "--device", str(a.device),
    "--report", str(report), "--screenshot", str(screenshot),"--smoke-repeat",str(a.repeat)]
env=os.environ.copy()
for key in list(env):
    if key.upper().startswith(('VK_','LWVK_')) or key.upper() in ('VULKAN_SDK','VK_SDK_PATH','PATH'):
        env.pop(key,None)
system=os.environ.get('SystemRoot',r'C:\Windows')
env['PATH']=system+'\\System32;'+system+';'+system+'\\System32\\Wbem'
if a.gpu_det_preprocess:
    if a.host_only:
        p.error('GPU DET experiment requires a physical GPU smoke test')
    env['LWVK_GPU_DET_PREPROCESS']='1'
completed = subprocess.run(args, cwd=output, env=env, timeout=180, capture_output=True, encoding="utf-8", errors="replace")
if completed.returncode:
    detail = report.read_text(encoding="utf-8") if report.is_file() else completed.stdout+completed.stderr
    raise RuntimeError(f"WinForms smoke exited {completed.returncode}: {detail}")
result = json.loads(report.read_text(encoding="utf-8"))
assert result["ok"] and screenshot.is_file()
assert result["mode"] == ("host-only" if a.host_only else "physical-vulkan")
if not a.host_only:
    assert result["device"] == a.device and result["full_items"] == 16
    assert result["title"] == result["roi_text"] == "纯臻营养护发素"
    assert result['lazy_details']=='JSON and grid passed'
    assert len(result['measurements'])==a.repeat
    assert result['measurements'][0]['first_call']
    for i,row in enumerate(result['measurements']):
        assert row['first_call']==(i==0)
        assert row['ui_ready_ms']>=row['client_ms']>=row['native_call_ms']>=row['timing']['total_ms']
print(json.dumps(result, ensure_ascii=False))
print(f"PASS: WinForms {prefix}, working directory is NOT the deployment directory; image: {screenshot}")
