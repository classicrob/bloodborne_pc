#!/usr/bin/env python3
"""SDK-only AJM decode test using LibAtrac9 and an analytic transform reference."""
import math
import os
from pathlib import Path
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'out/mac-playability'
OUT.mkdir(parents=True, exist_ok=True)
source = Path(os.environ.get('BB_ATRAC9_SOURCE_ROOT', str(ROOT)))
library = Path(os.environ.get('BB_ATRAC9_LIBRARY', str(ROOT / 'out/libatrac9.a')))
if not library.is_file():
    raise SystemExit('Build LibAtrac9 first or set BB_ATRAC9_LIBRARY and BB_ATRAC9_SOURCE_ROOT')
exe = OUT / 'test-ajm'
subprocess.run(['clang', '-arch', 'x86_64', '-std=c11', '-Wall', '-Wextra', '-Werror',
                '-I.', '-Isrc', '-I' + str(source), 'tests/test_ajm_macos.c',
                'src/compat_mac.c', str(library), '-o', str(exe)], cwd=ROOT, check=True)
pcm = OUT / 'ajm.pcm'
subprocess.run(['arch', '-x86_64', str(exe), str(pcm)], check=True)
actual = struct.unpack('<2048h', pcm.read_bytes())
# The synthesized stream has one nonzero spectral coefficient at index 4.
# Compute its DCT-IV directly, independently of LibAtrac9's FFT-like transform.
n = 256
mdct_window = [(math.sin(((i + .5) / n - .5) * math.pi) + 1) * .5 for i in range(n)]
window = [mdct_window[i] / (mdct_window[i]**2 + mdct_window[n-1-i]**2) for i in range(n)]
spectrum = 10000 * 65536 / 32767.5
transform = [spectrum * math.cos(math.pi / n * 4.5 * (k + .5)) for k in range(n)]
previous = [0.0] * n
reference = []
for frame in range(8):
    output = [0.0] * n
    for i in range(n // 2):
        output[i] = window[i] * transform[i+n//2] + previous[i]
        output[i+n//2] = -window[i+n//2] * transform[n-1-i] - previous[i+n//2]
        previous[i] = -window[n-1-i] * transform[n//2-i-1]
        previous[i+n//2] = window[n//2-i-1] * transform[i]
    reference.extend(max(-32768, min(32767, math.floor(value + .5))) for value in output)
maximum = max(abs(a-b) for a, b in zip(actual, reference))
assert maximum <= 1, f'ATRAC9 analytic PCM error: {maximum}'
print(f'PASS: 2048 ATRAC9 samples match analytic DCT-IV/overlap reference (max delta {maximum})')
