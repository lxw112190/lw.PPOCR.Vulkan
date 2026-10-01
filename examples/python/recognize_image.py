"""Recognition-only: pass an image of a cropped text line, not a full page."""
import argparse
import json
from pathlib import Path
import sys
import numpy as np
from PIL import Image
from lwvk import load, Network

for stream in (sys.stdout, sys.stderr):
    if hasattr(stream, "reconfigure"):
        stream.reconfigure(encoding="utf-8", errors="backslashreplace")
p = argparse.ArgumentParser(description=__doc__)
p.add_argument("--library", type=Path, required=True)
p.add_argument("--model", type=Path, required=True)
p.add_argument("--image", type=Path, required=True)
p.add_argument("--device", type=int, default=0)
p.add_argument("--roi", type=int, nargs=4, metavar=("X", "Y", "WIDTH", "HEIGHT"))
p.add_argument("--rec-width", type=int, default=0, help="0=adaptive, otherwise multiple of 8 in 32..960")
a = p.parse_args()
with Image.open(a.image) as source:
    image = source.convert("RGB")
if a.roi:
    x, y, w, h = a.roi
    if x < 0 or y < 0 or w <= 0 or h <= 0 or x+w > image.width or y+h > image.height:
        p.error("ROI is outside image")
    image = image.crop((x, y, x+w, y+h))
bgr = np.ascontiguousarray(np.asarray(image)[:, :, ::-1])
lib = load(a.library)
with Network(lib, a.model, a.device) as net:
    text, score, elapsed = net.recognize_bgr(bgr, a.rec_width)
    print(json.dumps(dict(text=text, score=score, gpu_inference_ms=elapsed,
        operation="recognition-only", width=image.width, height=image.height), ensure_ascii=False))
