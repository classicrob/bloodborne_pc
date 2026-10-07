#!/usr/bin/env python3
"""Compare one local FSB5 ATRAC9 stream; never copies game data into the repository."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess

p = argparse.ArgumentParser()
p.add_argument('bank', type=Path)
p.add_argument('--stream', type=int, default=0)
p.add_argument('--decoder', type=Path, required=True)
p.add_argument('--ffmpeg', default='ffmpeg')
p.add_argument('--out', type=Path, default=Path('out/mac-playability/asset'))
a = p.parse_args()
a.out.mkdir(parents=True, exist_ok=True)
bank = a.bank.read_bytes()
assert bank[:4] == b'FSB5'
version, count, headers, names, size, codec = struct.unpack_from('<6I', bank, 4)
assert version == 1 and codec == 13 and 0 <= a.stream < count
assert 60 + headers + names + size == len(bank)
# FSB5 fields follow the vgmstream project's src/meta/fsb5.c parser.
pos, streams = 60, []
for i in range(count):
    mode, = struct.unpack_from('<Q', bank, pos)
    pos += 8
    offset = ((mode >> 7) & 0x7ffffff) * 32
    channels = [1, 2, 6, 8][(mode >> 5) & 3]
    rate = [4000, 8000, 11000, 11025, 16000, 22050, 24000, 32000, 44100, 48000, 96000][(mode >> 1) & 15]
    config = None
    more = mode & 1
    while more:
        chunk, = struct.unpack_from('<I', bank, pos)
        more, length, kind = chunk & 1, (chunk >> 1) & 0xffffff, chunk >> 25
        pos += 4
        payload = bank[pos:pos+length]
        assert len(payload) == length
        pos += length
        if kind == 1: channels = payload[0]
        if kind == 2: rate, = struct.unpack('<I', payload)
        if kind == 9:
            if len(payload) >= 8 and payload[0] != 0xfe: payload = payload[4:]
            assert len(payload) == 4, 'This diagnostic only supports a single ATRAC9 layer'
            config = payload
    assert config is not None
    streams.append((offset, channels, rate, mode >> 34, config))
assert pos == 60 + headers
start, channels, rate, samples, config = streams[a.stream]
end = streams[a.stream+1][0] if a.stream+1 < count else size
value = int.from_bytes(config, 'big')
frame_bytes = ((value >> 5) & 2047) + 1
frames = 1 << ((value >> 3) & 3)
superframe = frame_bytes * frames
# FSB data entries are aligned to 32 bytes; discard only trailing alignment padding.
raw = bank[60+headers+names+start:60+headers+names+end]
padding = len(raw) % superframe
assert padding < 32, 'Unexpected partial superframe beyond FSB alignment'
if padding:
    assert not any(raw[-padding:]), 'Nonzero trailing FSB alignment bytes'
    raw = raw[:-padding]
(a.out/'input.bin').write_bytes(config + raw)
subprocess.run([str(a.decoder.resolve()), str(a.out/'input.bin'), str(a.out/'ajm.pcm')], check=True)
# ATRAC9 WAVEFORMATEXTENSIBLE: GUID, version, config, reserved; no encoder-delay trimming.
fmt = struct.pack('<HHIIHHHHI', 0xfffe, channels, rate, 0, superframe, 0, 34, 0, 4 if channels == 1 else 3)
fmt += bytes.fromhex('d242e147ba368d4d88fc61654f8c836c') + struct.pack('<I', 1) + config + bytes(4)
def chunk(kind, payload):
    return kind + struct.pack('<I', len(payload)) + payload + bytes(len(payload) & 1)
wave = b'WAVE' + chunk(b'fmt ', fmt) + chunk(b'data', raw)
(a.out/'input.at9').write_bytes(b'RIFF' + struct.pack('<I', len(wave)) + wave)
subprocess.run([a.ffmpeg, '-v', 'error', '-y', '-i', str(a.out/'input.at9'), '-f', 's16le', str(a.out/'ffmpeg.pcm')], check=True)
def pcm(path):
    data = path.read_bytes()
    return struct.unpack('<' + str(len(data)//2) + 'h', data)
x, y = pcm(a.out/'ajm.pcm'), pcm(a.out/'ffmpeg.pcm')
assert len(x) == len(y)
energy = sum(b*b for b in y)
gain = sum(v*b for v, b in zip(x, y))/energy if energy else 0
result = dict(bank=a.bank.name, bank_sha256=hashlib.sha256(bank).hexdigest(), stream=a.stream,
              config=config.hex(), channels=channels, rate=rate, nominal_samples=samples,
              samples_compared=len(x), max_difference=max(abs(v-b) for v,b in zip(x,y)),
              ajm_relative_gain=gain, residual_rms=math.sqrt(sum((v-gain*b)**2 for v,b in zip(x,y))/len(x)),
              max_residual_negative_four=max(abs(v+4*b) for v,b in zip(x,y)))
(a.out/'comparison.json').write_text(json.dumps(result, indent=2)+'\n')
print(json.dumps(result, indent=2))
