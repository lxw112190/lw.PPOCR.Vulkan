"""Full Vulkan OCR from an image, with optional box visualization (Pillow/NumPy)."""
import argparse
import json
from pathlib import Path
import sys
import numpy as np
from PIL import Image, ImageDraw
from lwvk import load, OCR
for stream in (sys.stdout, sys.stderr):
    if hasattr(stream, "reconfigure"):
        stream.reconfigure(encoding="utf-8", errors="backslashreplace")
p = argparse.ArgumentParser(description=__doc__)
p.add_argument("--library", type=Path, required=True)
p.add_argument("--models", type=Path, required=True)
p.add_argument("--image", type=Path, required=True)
p.add_argument("--device", type=int, default=0)
p.add_argument("--no-cls", action="store_true")
p.add_argument("--draw", type=Path)
a = p.parse_args()
with Image.open(a.image) as source:
    image = source.convert("RGB")
with OCR(load(a.library), a.models, a.device, enable_classifier=int(not a.no_cls)) as engine:
    result = engine.run(np.ascontiguousarray(np.asarray(image)[:, :, ::-1]))
print(json.dumps(result, ensure_ascii=False, indent=2))
if a.draw:
    draw = ImageDraw.Draw(image)
    for item in result["items"]:
        points = [(item[f"x{i}"], item[f"y{i}"]) for i in range(1, 5)]
        draw.line(points + [points[0]], fill="#ff3030", width=2)
    image.save(a.draw)
