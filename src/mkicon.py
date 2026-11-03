"""Собирает src/dkeys.ico из src/icon.png (буквы D и K на прозрачном фоне, 512x512).
Запуск: python src/mkicon.py (нужен Pillow). Каждый размер уменьшается заново, кадры хранятся как PNG;
большой кадр - с палитрой, чтобы не раздувать exe."""
import io, os, struct
from PIL import Image

SIZES = [16, 20, 24, 32, 48, 256]
here = os.path.dirname(os.path.abspath(__file__))
master = Image.open(os.path.join(here, 'icon.png')).convert('RGBA')

def frame(size):
    return master.convert('RGBa').resize((size, size), Image.LANCZOS).convert('RGBA')   # через premultiplied - без тёмной каймы

def png(img):
    if img.width >= 128:
        img = img.quantize(256, method=Image.FASTOCTREE)
    b = io.BytesIO(); img.save(b, 'PNG', optimize=True); return b.getvalue()

frames = [(s, png(frame(s))) for s in SIZES]
out = struct.pack('<HHH', 0, 1, len(frames))
off = 6 + 16 * len(frames)
for s, data in frames:
    out += struct.pack('<BBBBHHII', s % 256, s % 256, 0, 0, 1, 32, len(data), off)
    off += len(data)
out += b''.join(d for _, d in frames)
with open(os.path.join(here, 'dkeys.ico'), 'wb') as f:
    f.write(out)
print('dkeys.ico', len(out), 'bytes', [(s, len(d)) for s, d in frames])
