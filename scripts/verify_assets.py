"""Verify pinned vendored sources/models; no network and no hash auto-update."""
import hashlib
import json
from pathlib import Path

root=Path(__file__).resolve().parents[1]
lock=json.loads((root/"dependencies.lock.json").read_text(encoding="utf-8"))
for item in lock["files"]:
    path=root/item["path"]
    data=path.read_bytes()
    if item.get("normalize_lf"):
        data=data.replace(b"\r\n",b"\n").replace(b"\r",b"\n")
    actual=hashlib.sha256(data).hexdigest()
    if actual!=item["sha256"]:
        raise SystemExit(f"SHA-256 mismatch: {item['path']}: expected {item['sha256']}, got {actual}")
model=json.loads((root/"models/ppocrv6-tiny/det.json").read_text(encoding="utf-8"))
if hashlib.sha256((root/"models/ppocrv6-tiny/weights.bin").read_bytes()).hexdigest()!=model["weights_sha256"]:
    raise SystemExit("model manifest weights SHA-256 mismatch")
if model["format_version"]!=0 or len(model["nodes"])!=242:
    raise SystemExit("Tiny DET conversion contract mismatch")
print(f"PASS: {len(lock['files'])} pinned dependency/model files")
