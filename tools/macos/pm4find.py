#!/usr/bin/env python3
"""Usage: python3 tools/macos/pm4find.py out/replay/<capture> 0xADDR [CONTEXT=10]
Finds the WRITE_DATA packets in a perf replay capture (docs/macos/capture-replay.md) whose destination covers
ADDR, with CONTEXT packets before and after each, and the guest view (views.bin) that holds ADDR."""
import struct, sys
NAMES = {0x10:'NOP',0x11:'SET_BASE',0x12:'CLEAR_STATE',0x13:'INDEX_BUFFER_SIZE',0x15:'DISPATCH_DIRECT',0x16:'DISPATCH_INDIRECT',
 0x1F:'ATOMIC_GDS',0x22:'COND_EXEC',0x23:'PRED_EXEC',0x25:'DRAW_INDEX_INDIRECT_MULTI',0x26:'DRAW_INDEX_2',0x27:'CONTEXT_CONTROL',
 0x28:'DRAW_INDEX_OFFSET',0x2A:'INDEX_TYPE',0x2C:'DRAW_INDEX_MULTI_AUTO',0x2D:'DRAW_INDEX_AUTO',0x2F:'NUM_INSTANCES',0x33:'INDIRECT_BUFFER_CONST',
 0x34:'STRMOUT_BUFFER_UPDATE',0x35:'DRAW_INDEX_OFFSET_2',0x37:'WRITE_DATA',0x38:'DRAW_INDEX_INDIRECT_MULTI',0x3A:'MEM_SEMAPHORE',0x3C:'WAIT_REG_MEM',
 0x3F:'INDIRECT_BUFFER',0x40:'COPY_DATA',0x41:'CP_DMA',0x42:'PFP_SYNC_ME',0x43:'SURFACE_SYNC',0x46:'EVENT_WRITE',0x47:'EVENT_WRITE_EOP',
 0x48:'EVENT_WRITE_EOS',0x49:'RELEASE_MEM',0x4A:'PREAMBLE_CNTL',0x50:'DMA_DATA',0x58:'ACQUIRE_MEM',0x59:'REWIND',0x5E:'LOAD_UCONFIG_REG',
 0x5F:'LOAD_SH_REG',0x60:'LOAD_CONFIG_REG',0x61:'LOAD_CONTEXT_REG',0x68:'SET_CONFIG_REG',0x69:'SET_CONTEXT_REG',0x6D:'SET_CONTEXT_REG_INDIRECT',
 0x76:'SET_SH_REG',0x77:'SET_SH_REG_OFFSET',0x79:'SET_UCONFIG_REG',0x80:'LOAD_CONST_RAM',0x81:'WRITE_CONST_RAM',0x83:'DUMP_CONST_RAM',
 0x84:'INCREMENT_CE_COUNTER',0x85:'INCREMENT_DE_COUNTER',0x86:'WAIT_ON_CE_COUNTER',0x88:'WAIT_ON_DE_COUNTER_DIFF',0x8B:'SWITCH_BUFFER'}
def name(op): return NAMES.get(op, f'op_{op:#04x}')
def header(f, magic):
    m, ver, _, recs = struct.unpack('<8sIIQ', f.read(24))
    assert m == magic, (m, magic); return recs
def describe(words, i):
    h = words[i]; t = h >> 30
    if t != 3: return f'type{t} {h:#010x}', 1
    count = (h >> 16) & 0x3fff; op = (h >> 8) & 0xff; n = count + 2
    body = words[i+1:i+n]; extra = ''
    if op == 0x37 and len(body) >= 3:
        ctl = body[0]; dst = body[1] | (body[2] << 32)
        extra = f' dst_sel {(ctl>>8)&0xf} wr_confirm {(ctl>>20)&1} engine {(ctl>>30)&3} dst {dst:#x} dwords {len(body)-3} data[0..3] {[hex(x) for x in body[3:7]]}'
    elif op == 0x3C and len(body) >= 6:
        fn = body[0]; addr = body[1] | (body[2] << 32)
        extra = f' function {fn&7} mem_space {(fn>>4)&3} engine {(fn>>8)&1} poll {addr:#x} ref {body[3]:#x} mask {body[4]:#x}'
    elif op in (0x47, 0x49, 0x48) and len(body) >= 4:
        extra = ' ' + ' '.join(f'{x:#x}' for x in body[:6])
    elif op in (0x3F, 0x33) and len(body) >= 3:
        extra = f' ib {(body[0] | (body[1]&0xffff)<<32):#x} size_dw {body[2]&0xfffff}'
    return f'{name(op)} (count {count}){extra}', n
def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    cap, dst = sys.argv[1], int(sys.argv[2], 0); ctx = int(sys.argv[3]) if len(sys.argv) > 3 else 10
    with open(f'{cap}/views.bin', 'rb') as f:
        header(f, b'BBRPVIEW'); blocks, views = struct.unpack('<QQ', f.read(16))
        f.read(blocks * 24)
        for _ in range(views):
            s, e, phys, flex, prot, typ, _r = struct.unpack('<QQQIiiI', f.read(40))
            if s <= dst < e: print(f'view covering {dst:#x}: [{s:#x}, {e:#x}) size {e-s:#x} phys {phys:#x} {"flexible" if flex else "direct"} prot {prot} type {typ}')
    with open(f'{cap}/submissions.bin', 'rb') as f:
        subs = header(f, b'BBRPSUBS'); hits = 0
        for si in range(subs):
            num, nreg, dcb, ccb, _ = struct.unpack('<QIiiI', f.read(24))
            for ri in range(nreg):
                guest, size_dw, const, copied, _ = struct.unpack('<QIIII', f.read(24))
                words = list(struct.unpack(f'<{size_dw}I', f.read(size_dw * 4))) if copied else []
                pk = []; i = 0
                while i < len(words):
                    d, n = describe(words, i); pk.append((i, d, n)); i += n
                for k, (off, d, n) in enumerate(pk):
                    h = words[off]
                    if h >> 30 == 3 and ((h >> 8) & 0xff) == 0x37 and n >= 4:
                        b = words[off+1:off+n]; a = b[1] | (b[2] << 32); size = (n - 4) * 4
                        if a <= dst < a + max(size, 4):
                            hits += 1
                            print(f'\n=== hit {hits}: submission {si} (submit #{num}) region {ri} {"DCB" if ri == dcb else "CCB" if ri == ccb else "IB"} guest {guest:#x} size_dw {size_dw}{" const" if const else ""}; packet at dword {off} (guest {guest + off*4:#x})')
                            for j in range(max(0, k-ctx), min(len(pk), k+ctx+1)):
                                o, dd, _n = pk[j]
                                print(f'  {"->" if j == k else "  "} dw {o:6d} {guest+o*4:#x}: {dd}')
        print(f'\n{hits} WRITE_DATA packets cover {dst:#x}')
if __name__ == "__main__":
    main()
