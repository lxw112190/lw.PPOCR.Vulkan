"""Display/export a Tiny DET probability map. This does not recognize text."""
import argparse
from pathlib import Path
from PIL import Image
import numpy as np
from lwvk import load, Detector

p=argparse.ArgumentParser(description=__doc__)
p.add_argument("--library",type=Path,required=True)
p.add_argument("--model",type=Path,required=True)
p.add_argument("--image",type=Path,required=True)
p.add_argument("--output",type=Path,default=Path("det-map.png"))
p.add_argument("--device",type=int,default=0)
p.add_argument("--max-side",type=int,default=640)
a=p.parse_args()
if a.max_side<32 or a.max_side>960 or a.max_side%32:
    p.error("max-side must be a multiple of 32 in 32..960")
image=Image.open(a.image).convert("RGB")
ratio=min(1.0,a.max_side/max(image.size))
w,h=(max(32,min(a.max_side,int(round(d*ratio/32))*32)) for d in image.size)
rgb=np.asarray(image.resize((w,h),Image.Resampling.BILINEAR),dtype=np.float32)/255
value=((rgb-np.array([.485,.456,.406],dtype=np.float32))/np.array([.229,.224,.225],dtype=np.float32)).transpose(2,0,1)[None]
with Detector(load(a.library),a.model,a.device) as detector:
    output,ms=detector.run(value)
a.output.parent.mkdir(parents=True,exist_ok=True)
Image.fromarray((np.clip(output[0,0],0,1)*255).astype(np.uint8)).save(a.output)
print(f"DET map {w}x{h}, inference {ms:.3f} ms (plan construction excluded), saved {a.output}")
print("DET map only; this example does not run DB boxes/CLS/REC/full OCR.")
