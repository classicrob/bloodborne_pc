#!/usr/bin/env python3
"""Usage: python3 tools/macos/pm4dump.py out/replay/<capture> 0xGUEST_ADDR [BEFORE=12] [AFTER=12]
Dumps the PM4 packets around the packet at GUEST_ADDR in the captured command buffer that holds it (opcode names,
WRITE_DATA / WAIT_REG_MEM / EOP fields, as pm4find.py)."""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pm4find import describe, header


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    cap, at = sys.argv[1], int(sys.argv[2], 0)
    before = int(sys.argv[3]) if len(sys.argv) > 3 else 12
    after = int(sys.argv[4]) if len(sys.argv) > 4 else 12
    found = False
    with open(f'{cap}/submissions.bin', 'rb') as f:
        subs = header(f, b'BBRPSUBS')
        for si in range(subs):
            num, nreg, dcb, ccb, _ = struct.unpack('<QIiiI', f.read(24))
            for ri in range(nreg):
                guest, size_dw, const, copied, _ = struct.unpack('<QIIII', f.read(24))
                words = list(struct.unpack(f'<{size_dw}I', f.read(size_dw * 4))) if copied else []
                if not (guest <= at < guest + size_dw * 4) or not words:
                    continue
                packets, i = [], 0
                while i < len(words):
                    text, n = describe(words, i)
                    packets.append((i, text))
                    i += n
                k = next(j for j, (o, _) in enumerate(packets) if guest + o * 4 >= at)
                found = True
                print(f'submission {si} (submit #{num}) region {ri} guest {guest:#x} size_dw {size_dw}; packet index {k}')
                for j in range(max(0, k - before), min(len(packets), k + after + 1)):
                    o, text = packets[j]
                    print(f'  {"->" if j == k else "  "} dw {o:6d} {guest + o * 4:#x}: {text}')
    if not found:
        sys.exit(f'STOP: no captured command buffer holds {at:#x}')


if __name__ == '__main__':
    main()
