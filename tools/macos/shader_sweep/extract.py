#!/usr/bin/env python3
"""Extracts every PS4 shader the game ships (dvdroot_ps4/shader/*.shaderbnd*.dcx) into a corpus.

Each archive is a DCX-compressed BND4 binder of *.vpo/*.ppo/*.gpo/*.cpo/*.hpo/*.dpo files. A file is
a FromSoftware header (magic 0x0003e220, then the low half of the shader hash), then a PSSL .sb
shader: the "Shdr" header (stage, header size in dwords), the Gnmx stage struct (stage registers,
input usage slots, vertex input/export semantics), the GCN code and its "OrbShdr" binary info, whose
shader_hash is the hash the game logs and BB_DUMP_SHADERS print. A GS file also embeds its copy VS
(struct and code; same hash). Every stage layout below must fill the header exactly, or the file
stops the extraction.

Writes OUT/jobs.txt (one line per distinct (hash, stage), the shader-sweep driver's input) and
OUT/code/<hash>-<stage>[-copy].bin (code plus its binary info). The game data is only read.
  python3 -I tools/macos/shader_sweep/extract.py GAME_DIR OUT
"""
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fromsoft  # noqa: E402

STAGES = {1: 'vs', 2: 'ps', 3: 'gs', 4: 'cs', 5: 'es', 6: 'ls', 7: 'hs'}
VCC_HI_TOKEN = 0xBEEB03FF
BINARY_INFO_BYTES = 28
# Stage struct size in dwords before the input usage slots (common data at word 0).
SLOTS_AT = {'vs': 10, 'ps': 15, 'cs': 10, 'es': 8, 'ls': 8, 'hs': 16, 'gs': 14}


def footers(blob):
    pos = blob.find(b'OrbShdr')
    while pos != -1:
        word, = struct.unpack_from('<I', blob, pos + 8)
        hash_lo, hash_hi = struct.unpack_from('<II', blob, pos + 16)
        yield pos, word >> 8, hash_lo | (hash_hi << 32)
        pos = blob.find(b'OrbShdr', pos + 1)


def code_at(blob, footer, length, after):
    """Code (plus binary info) whose binary info is at `footer`: from the s_mov_b32 vcc_hi token
    that points at it (the production SearchBinaryInfo fast path), else `length` bytes back."""
    start = footer - length
    for pos in range(after, footer, 4):
        token, literal = struct.unpack_from('<II', blob, pos)
        if token == VCC_HI_TOKEN and pos + (literal + 1) * 8 == footer:
            start = pos
            break
    return blob[start:footer + BINARY_INFO_BYTES], length // 4


def vertex_struct(words, at, stage, name):
    """VsShader at word `at`: common, PGM_LO/HI, RSRC1/2, SPI_VS_OUT_CONFIG, SPI_SHADER_POS_FORMAT,
    PA_CL_VS_OUT_CNTL, counts, then slots, input semantics (1 dword each), export semantics
    (2 bytes each). Returns (regs, slots, semantics, words used)."""
    n_slots = words[at] >> 24
    n_in, n_exp = words[at + 9] & 0xFF, (words[at + 9] >> 8) & 0xFF
    regs = {'rsrc1': words[at + 4], 'rsrc2': words[at + 5], 'vs_out_config': words[at + 6],
            'pos_format': words[at + 7], 'vs_out_cntl': words[at + 8]}
    slots = words[at + 10:at + 10 + n_slots]
    semantics = words[at + 10 + n_slots:at + 10 + n_slots + n_in]
    return regs, slots, semantics, 10 + n_slots + n_in + (n_exp + 1) // 2


def parse(blob, name):
    if struct.unpack_from('<I', blob, 0)[0] != 0x20E20300 or blob[0x24:0x28] != b'Shdr':
        raise ValueError(f'{name}: not a FromSoftware-wrapped PSSL shader')
    stage = STAGES[blob[0x2C]]
    header_dw = blob[0x2D]
    w = struct.unpack_from('<%dI' % header_dw, blob, 0x34)
    n_slots = w[0] >> 24
    at = SLOTS_AT[stage]
    slots = w[at:at + n_slots]
    semantics = ()
    used = at + n_slots
    copy = None
    if stage == 'vs':
        regs, slots, semantics, used = vertex_struct(w, 0, stage, name)
    else:
        regs = {'rsrc1': w[4], 'rsrc2': w[5]}
    if stage == 'cs':
        regs.update(threads_x=w[6], threads_y=w[7], threads_z=w[8])
    elif stage == 'ps':
        regs.update(z_format=w[6], col_format=w[7], input_ena=w[8], input_addr=w[9],
                    in_control=w[10], baryc_cntl=w[11], db_shader_control=w[12],
                    cb_shader_mask=w[13])
        # Pixel input semantics, 2 bytes each: semantic, default value (bits 8-9), flat (bit 10).
        n_in = w[14] & 0xFF
        semantics = struct.unpack_from('<%dH' % n_in, blob, 0x34 + (used) * 4)
        used += (n_in + 1) // 2
    elif stage in ('es', 'ls'):
        semantics = w[used:used + (w[6] & 0xFF)]
        used += len(semantics)
        if stage == 'es':
            regs.update(esgs_dwords=w[7])
        else:
            regs.update(ls_stride=w[7])
    elif stage == 'hs':
        regs.update(tf_param=w[6], hull=' '.join('%x' % x for x in w[9:16]))
    elif stage == 'gs':
        regs.update(strmout_config=w[6], out_prim_type=w[7], instance_cnt=w[8],
                    vert_itemsize0=w[9], vert_itemsize1=w[10], vert_itemsize2=w[11],
                    vert_itemsize3=w[12], max_vert_out=w[13])
        copy_regs, copy_slots, _, copy_used = vertex_struct(w, used, 'vs', name)
        copy = {'regs': copy_regs, 'slots': copy_slots}
        used += copy_used
    if used != header_dw:
        raise ValueError(f'{name}: {stage} struct uses {used} of {header_dw} header dwords')

    found = list(footers(blob))
    code_start = 0x34 + header_dw * 4
    footer, length, shader_hash = next(f for f in found if f[0] > code_start)
    entry = {'stage': stage, 'hash': shader_hash, 'regs': regs, 'slots': slots,
             'semantics': semantics}
    entry['code'], entry['dwords'] = code_at(blob, footer, length, code_start)
    if copy:
        copy_footer, copy_length, copy_hash = next(f for f in found if f[0] > footer)
        if copy_hash != shader_hash:
            raise ValueError(f'{name}: copy shader hash {copy_hash:#x} != GS {shader_hash:#x}')
        copy['code'], copy['dwords'] = code_at(blob, copy_footer, copy_length,
                                               footer + BINARY_INFO_BYTES)
        entry['copy'] = copy
    return entry


# Job order: an LS compiles against a hull, so every HS runs before the first LS.
KIND_ORDER = ['cs', 'vs', 'ps', 'es', 'gs', 'hs', 'ds', 'ls']


def stem(label):
    """'archive.shaderbnd.dcx:dir\\Name.gpo' -> ('archive', 'Name')"""
    archive, name = label.split(':', 1)
    return archive.split('.')[0], re.sub(r'\.[a-z]po$', '', name.split('\\')[-1])


def es_pairs(seen):
    """GS hash -> {ESGS item size: [ES hashes]} for the ES shipped under the GS's name
    (GS_X.gpo with VS_X.vpo, or Name.gpo with Name.vpo, in the same archive)."""
    es_by_stem = {}
    for (shader_hash, stage), e in seen.items():
        if stage == 'es':
            for label in e['names']:
                es_by_stem.setdefault(stem(label), set()).add((shader_hash, e['regs']['esgs_dwords']))
    pairs = {}
    for (shader_hash, stage), e in seen.items():
        if stage != 'gs':
            continue
        sizes = pairs.setdefault(shader_hash, {})
        for label in e['names']:
            archive, name = stem(label)
            for candidate in ((archive, re.sub('^GS_', 'VS_', name)), (archive, name)):
                for es_hash, size in es_by_stem.get(candidate, ()):
                    sizes.setdefault(size, set()).add(es_hash)
    return pairs


def job_lines(key, e, out, pairs):
    shader_hash, stage = key
    kind = 'ds' if stage == 'vs' and e['names'][0].endswith('.dpo') else stage
    fields = [f'{shader_hash:#x}', kind, os.path.join(out, 'code', '%016x-%s.bin' % key),
              str(e['dwords'])]
    fields += [f'{k}={v:#x}' if isinstance(v, int) else f'{k}={v.replace(" ", ",")}'
               for k, v in e['regs'].items()]
    fields += [f'slot={s:#010x}' for s in e['slots']]
    fields += [f'sem={s:#010x}' for s in e['semantics']]
    if 'copy' in e:
        c = e['copy']
        fields += ['copy_code=' + os.path.join(out, 'code', '%016x-%s-copy.bin' % key),
                   f"copy_dwords={c['dwords']}"]
        fields += [f'copy_{k}={v:#x}' for k, v in c['regs'].items()]
        fields += [f'copy_slot={s:#010x}' for s in c['slots']]
    fields += ['name=' + e['names'][0].replace(' ', '_'), f"files={len(e['names'])}"]
    if stage != 'gs':
        return [(kind, ' '.join(fields))]
    # One GS job per ESGS item size its ES use. A GS without an ES of its name gets the size of
    # its own output vertex, stated as such in the job.
    sizes = pairs.get(shader_hash) or {e['regs']['vert_itemsize0']: {'unpaired'}}
    return [(kind, ' '.join(fields + [f'esgs={size:#x}', 'es=' + ','.join(
        h if isinstance(h, str) else f'{h:#x}' for h in sorted(es, key=str))]))
            for size, es in sorted(sizes.items())]


def main():
    game, out = sys.argv[1], os.path.abspath(sys.argv[2])
    shader_dir = os.path.join(game, 'dvdroot_ps4', 'shader')
    os.makedirs(os.path.join(out, 'code'), exist_ok=True)
    seen = {}
    files = 0
    for archive in sorted(os.listdir(shader_dir)):
        with open(os.path.join(shader_dir, archive), 'rb') as f:
            binder = fromsoft.dcx_decompress(f.read())
        for name, _, blob in fromsoft.bnd4_files(binder):
            files += 1
            label = f'{archive}:{name}'
            entry = parse(blob, label)
            key = (entry['hash'], entry['stage'])
            if key in seen:
                if seen[key]['code'] != entry['code']:
                    raise ValueError(f'{label}: same hash and stage as {seen[key]["names"][0]}, different code')
                seen[key]['names'].append(label)
                continue
            entry['names'] = [label]
            seen[key] = entry
            base = os.path.join(out, 'code', '%016x-%s' % key)
            with open(base + '.bin', 'wb') as f:
                f.write(entry['code'])
            if 'copy' in entry:
                with open(base + '-copy.bin', 'wb') as f:
                    f.write(entry['copy']['code'])
    pairs = es_pairs(seen)
    jobs = [line for key, e in sorted(seen.items()) for line in job_lines(key, e, out, pairs)]
    jobs.sort(key=lambda job: KIND_ORDER.index(job[0]))
    with open(os.path.join(out, 'jobs.txt'), 'w') as f:
        f.writelines(line + '\n' for _, line in jobs)
    print(f'{files} shader files, {len(seen)} distinct (hash, stage), {len(jobs)} jobs -> {out}/jobs.txt')


if __name__ == '__main__':
    main()
