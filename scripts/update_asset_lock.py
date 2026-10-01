"""Maintainer-only deterministic hash generation AFTER source review, never in CI."""
import hashlib
import json
from pathlib import Path

root=Path(__file__).resolve().parents[1]
paths=list((root/"third_party/simd-paddleocr/shaders").glob("*.comp"))
paths += [root/"third_party/nlohmann/json.hpp",root/"LICENSE",root/"licenses/nlohmann-json-MIT.txt",
          root/"licenses/PaddleOCR-models-APACHE-2.0.txt",root/"models/ppocrv6-tiny/det.onnx",
          root/"models/ppocrv6-tiny/det.json",root/"models/ppocrv6-tiny/weights.bin",root/"test-images/sample.jpg"]
paths += [root/"licenses/lw-PPOCR-C-MIT.txt"]
for task in ("cls", "rec"):
    paths += [root/f"models/ppocrv6-tiny/{task}/{name}" for name in ("source.onnx", "model.json", "weights.bin")]
paths += [root/"models/ppocrv6-tiny/rec/dictionary.txt"]
paths += list((root/"third_party/lw-ppocr-c").rglob("*.c")) + list((root/"third_party/lw-ppocr-c").rglob("*.h"))
files=[]
for path in sorted(paths,key=lambda x:x.relative_to(root).as_posix()):
    data=path.read_bytes()
    normalize=path.suffix in (".hpp",".txt", ".json", ".c", ".h") or path.name=="LICENSE"
    if path.name == "dictionary.txt": normalize=False
    if normalize: data=data.replace(b"\r\n",b"\n").replace(b"\r",b"\n")
    files.append({"path":path.relative_to(root).as_posix(),"sha256":hashlib.sha256(data).hexdigest(),"normalize_lf":normalize})
lock={"lock_version":1,"simd_paddleocr":{"repository":"https://github.com/sdcb/SimdPaddleOCR",
       "commit":"6298596e28404f0e93af585a79e84086363eeb72","license":"Apache-2.0"},
       "lw_ppocr_c":{"repository":"https://github.com/lxw112190/lw.PPOCR.C","commit":"b7d2b42383adcee68b0e4f1a7c3de546be9c9552","license":"MIT"},
       "nlohmann_json":{"version":"3.12.0","license":"MIT"},
       "vulkan_sdk":{"windows_version":"1.4.350.0","windows_sha256":"855b27ba05d2d8119c5114c5d4ff870ca38f2c632b11e1bb9923b9b7e6ecfe7b",
       "linux_version":"1.4.350.0","linux_sha256":"b65f068ab36263559da49d7cacd7e7b9df23824ca8b68ccc522a2b06f5725df2",
       "source":"https://vulkan.lunarg.com/sdk/home"},"files":files}
(root/"dependencies.lock.json").write_text(json.dumps(lock,indent=2)+"\n",encoding="utf-8")
print("Updated reviewed asset lock. Inspect the diff before committing.")
