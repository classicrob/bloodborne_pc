#!/usr/bin/env python3
"""Usage: python3 tools/macos/drainseg.py out/replay/<capture> 0xDCB_GUEST
Split one captured command buffer at its self-polling
WAIT_REG_MEMs (poll address inside the same buffer) and count the packets of each segment."""
import collections, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pm4find import describe, header

if len(sys.argv) < 3:
    sys.exit(__doc__)
cap, dcb = sys.argv[1], int(sys.argv[2], 0)
with open(f'{cap}/submissions.bin', 'rb') as f:
    subs = header(f, b'BBRPSUBS')
    for si in range(subs):
        num, nreg, _, _, _ = struct.unpack('<QIiiI', f.read(24))
        for ri in range(nreg):
            guest, size_dw, const, copied, _ = struct.unpack('<QIIII', f.read(24))
            words = list(struct.unpack(f'<{size_dw}I', f.read(size_dw * 4))) if copied else []
            if guest != dcb:
                continue
            end = guest + size_dw * 4
            seg, segs, i = collections.Counter(), [], 0
            while i < len(words):
                d, n = describe(words, i)
                op = d.split(' ')[0]
                seg[op] += 1
                if op == 'WAIT_REG_MEM' and 'poll ' in d:
                    poll = int(d.split('poll ')[1].split(' ')[0], 0)
                    if guest <= poll < end:
                        segs.append((guest + i * 4, seg))
                        seg = collections.Counter()
                i += n
            segs.append((end, seg))
            print(f'submission {si} (submit #{num}) DCB {guest:#x} {size_dw} dwords: {len(segs) - 1} self-polling waits')
            for k, (at, c) in enumerate(segs):
                label = f'up to self-polling WAIT at {at:#x}' if k < len(segs) - 1 else 'after the last one'
                draws = sum(v for o, v in c.items() if o.startswith('DRAW'))
                disp = sum(v for o, v in c.items() if o.startswith('DISPATCH'))
                top = ', '.join(f'{o} {v}' for o, v in c.most_common())
                print(f'  segment {k} ({label}): draws {draws}, dispatches {disp}, '
                      f'WRITE_DATA {c["WRITE_DATA"]}; {top}')
