"""absheet.py OUT.jpg RUN_A RUN_B : side-by-side captures (idle-1..5, walk-1..4) of two R0 runs, plus a
pixel-difference table: A vs B per idle capture, and the within-run idle drift as the noise baseline."""
import sys
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw
out, a, b = sys.argv[1], Path(sys.argv[2]), Path(sys.argv[3])
def shot(run, stage):
    p = sorted((run / 'shots').glob(f'*-{stage}.jpg'))
    return Image.open(p[-1]).convert('RGB').resize((480, 270)) if p else None
def mad(x, y):
    return float(np.abs(np.asarray(x, np.float32) - np.asarray(y, np.float32)).mean()) if x and y else float('nan')
stages = [f'idle-{i}' for i in range(1, 6)] + [f'walk-{i}' for i in range(1, 5)]
sheet = Image.new('RGB', (970, 280 * len(stages)), 'black'); d = ImageDraw.Draw(sheet)
for r, s in enumerate(stages):
    for c, run in enumerate((a, b)):
        im = shot(run, s)
        if im: sheet.paste(im, (c * 490, r * 280))
        d.text((c * 490 + 5, r * 280 + 5), f'{"AB"[c]} {s}', fill='yellow')
sheet.save(out, quality=85)
for s in stages[:5]:
    print(f'{s}: A vs B mean abs diff {mad(shot(a, s), shot(b, s)):.2f}')
for run, n in ((a, 'A'), (b, 'B')):
    print(f'{n} idle drift (idle-k vs idle-k+1): ' + ' '.join(f'{mad(shot(run, f"idle-{i}"), shot(run, f"idle-{i+1}")):.2f}' for i in range(1, 5)))
