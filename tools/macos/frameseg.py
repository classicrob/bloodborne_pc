#!/usr/bin/env python3
"""Usage: python3 tools/macos/frameseg.py out/replay/<capture>
Per captured submission region, dwords, draws, dispatches, WRITE_DATA,
EOP/EOS, WAIT_REG_MEM, NOP payload dwords (to see where a frame's work is)."""
import collections, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pm4find import describe, header

if len(sys.argv) < 2:
    sys.exit(__doc__)
cap = sys.argv[1]
total = collections.Counter()
with open(f'{cap}/submissions.bin', 'rb') as f:
    subs = header(f, b'BBRPSUBS')
    for si in range(subs):
        num, nreg, dcb, ccb, _ = struct.unpack('<QIiiI', f.read(24))
        for ri in range(nreg):
            guest, size_dw, const, copied, _ = struct.unpack('<QIIII', f.read(24))
            words = list(struct.unpack(f'<{size_dw}I', f.read(size_dw * 4))) if copied else []
            if not words:
                continue
            c, i, nop_dw = collections.Counter(), 0, 0
            while i < len(words):
                d, n = describe(words, i)
                op = d.split(' ')[0]
                c[op] += 1
                if op == 'Nop':
                    nop_dw += n
                i += n
            draws = sum(v for o, v in c.items() if o.startswith('Draw'))
            disp = sum(v for o, v in c.items() if o.startswith('Dispatch'))
            total['draws'] += draws
            total['dispatches'] += disp
            if draws or disp or c['WriteData'] or c['WaitRegMem']:
                print(f'sub {si:3d} #{num} region {ri} {guest:#x} {"CE " if const else ""}{size_dw:6d} dw: '
                      f'draws {draws}, dispatches {disp}, WRITE_DATA {c["WriteData"]}, '
                      f'EOP {c["EventWriteEop"]}, EOS {c["EventWriteEos"]}, WAIT {c["WaitRegMem"]}, '
                      f'IB {c["IndirectBuffer"]}, NOP dw {nop_dw}')
print('total', dict(total))
