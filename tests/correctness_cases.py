"""Portable derivative corpus, not a substitute for diverse real-world ground truth."""
import hashlib
from pathlib import Path
from PIL import Image, ImageEnhance

SOURCE_SHA256 = '30c417c9f758a3b62718729f5a944f7d2e10cdd2bde0e8ce6785523ddb68ffe9'
CORPUS_VERSION = 1

def cases(source: Path, quick=False, extended=False):
    assert hashlib.sha256(source.read_bytes()).hexdigest() == SOURCE_SHA256, 'Reviewed source image changed'
    with Image.open(source) as original:
        image = original.convert('RGB')
    if quick:
        image = image.resize((320, 320))
    result = [('sample', image), ('rotated-180', image.rotate(180))]
    if not quick:
        result += [('wide-resized', image.resize((640, 400))), ('small', image.resize((320, 320)))]
    if extended:
        # Keep the extended quick CI suite <=320 on each side for lavapipe.
        result += [
            ('rotated-90', image.rotate(90)),
            ('rotated-270', image.rotate(270)),
            ('wide-compressed', image.resize((320, 160))),
            ('portrait-compressed', image.resize((160, 320))),
            ('low-contrast', ImageEnhance.Contrast(image).enhance(.6)),
            ('tilted-9', image.rotate(9, resample=Image.Resampling.BICUBIC, fillcolor='white')),
            ('grayscale', image.convert('L').convert('RGB')),
            ('blank-dark', Image.new('RGB', (96, 64), 'black')),
        ]
    result += [('blank', Image.new('RGB', (96, 64), 'white'))]
    return result
