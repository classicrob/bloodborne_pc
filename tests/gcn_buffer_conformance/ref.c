/* GCN2 buffer access reference model; see SPEC.md for the quoted ISA rules and the readings. */
#include <math.h>
#include <string.h>
#include "ref.h"

static uint64_t m32(uint64_t x, unsigned reading) {
    return (reading & GCN_R_WRAP32) ? (uint32_t)x : x;
}

static uint32_t f32_bits(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

int gcn_format_bits(uint32_t dfmt, uint32_t* elem_bytes, uint32_t* ncomp, uint32_t shift[4], uint32_t width[4]) {
    uint32_t k = 0, cb = 0;
    if (gcn_format_layout(dfmt, &k, &cb)) {
        *elem_bytes = k * cb;
        *ncomp = k;
        for (uint32_t c = 0; c < k; c++) {
            shift[c] = c * cb * 8;
            width[c] = cb * 8;
        }
        return 1;
    }
    static const uint32_t packed[4][2][4] = {
        /* 10_11_11 */ {{0, 11, 22, 0}, {11, 11, 10, 0}},
        /* 11_11_10 */ {{0, 10, 21, 0}, {10, 11, 11, 0}},
        /* 10_10_10_2 */ {{0, 2, 12, 22}, {2, 10, 10, 10}},
        /* 2_10_10_10 */ {{0, 10, 20, 30}, {10, 10, 10, 2}},
    };
    if (dfmt < GCN_DFMT_10_11_11 || dfmt > GCN_DFMT_2_10_10_10) return 0;
    const uint32_t i = dfmt - GCN_DFMT_10_11_11;
    *elem_bytes = 4;
    *ncomp = i < 2 ? 3 : 4;
    for (uint32_t c = 0; c < 4; c++) {
        shift[c] = packed[i][0][c];
        width[c] = packed[i][1][c];
    }
    return 1;
}

static float small_float(uint32_t raw, uint32_t ebits, uint32_t mbits, int sign) {
    const uint32_t m = raw & ((1u << mbits) - 1), e = (raw >> mbits) & ((1u << ebits) - 1);
    const int neg = sign && ((raw >> (ebits + mbits)) & 1);
    const int bias = (1 << (ebits - 1)) - 1;
    double v;
    if (e == (1u << ebits) - 1) {
        v = m ? NAN : INFINITY;
    } else if (e == 0) {
        v = ldexp((double)m, 1 - bias - (int)mbits);
    } else {
        v = ldexp((double)(m | (1u << mbits)), (int)e - bias - (int)mbits);
    }
    return (float)(neg ? -v : v);
}

uint32_t gcn_convert_component(uint32_t raw, uint32_t width, uint32_t nfmt, int* unknown) {
    const uint64_t umax = width >= 32 ? 0xFFFFFFFFull : ((1ull << width) - 1);
    const int64_t sx = width >= 32 ? (int64_t)(int32_t)raw
                                   : ((raw >> (width - 1)) & 1) ? (int64_t)raw - (int64_t)(1ull << width) : (int64_t)raw;
    switch (nfmt) {
    case GCN_NFMT_UNORM: return f32_bits((float)((double)raw / (double)umax));
    case GCN_NFMT_SNORM: {
        const double q = (double)sx / (double)((1ull << (width - 1)) - 1);
        return f32_bits((float)(q < -1.0 ? -1.0 : q));
    }
    case GCN_NFMT_SNORM_OGL: return f32_bits((float)((2.0 * (double)sx + 1.0) / (double)umax));
    case GCN_NFMT_USCALED: return f32_bits((float)raw);
    case GCN_NFMT_SSCALED: return f32_bits((float)sx);
    case GCN_NFMT_UINT: return raw;
    case GCN_NFMT_SINT: return (uint32_t)(int32_t)sx;
    case GCN_NFMT_FLOAT:
        if (width == 32) return raw;
        if (width == 16) return f32_bits(small_float(raw, 5, 10, 1));
        if (width == 11) return f32_bits(small_float(raw, 5, 6, 0));
        if (width == 10) return f32_bits(small_float(raw, 5, 5, 0));
        break;
    default: break;
    }
    *unknown = 1;
    return 0;
}

int gcn_format_layout(uint32_t dfmt, uint32_t* ncomp, uint32_t* comp_bytes) {
    switch (dfmt) {
    case GCN_DFMT_8: *ncomp = 1; *comp_bytes = 1; return 1;
    case GCN_DFMT_16: *ncomp = 1; *comp_bytes = 2; return 1;
    case GCN_DFMT_8_8: *ncomp = 2; *comp_bytes = 1; return 1;
    case GCN_DFMT_32: *ncomp = 1; *comp_bytes = 4; return 1;
    case GCN_DFMT_16_16: *ncomp = 2; *comp_bytes = 2; return 1;
    case GCN_DFMT_8_8_8_8: *ncomp = 4; *comp_bytes = 1; return 1;
    case GCN_DFMT_32_32: *ncomp = 2; *comp_bytes = 4; return 1;
    case GCN_DFMT_16_16_16_16: *ncomp = 4; *comp_bytes = 2; return 1;
    case GCN_DFMT_32_32_32: *ncomp = 3; *comp_bytes = 4; return 1;
    case GCN_DFMT_32_32_32_32: *ncomp = 4; *comp_bytes = 4; return 1;
    default: return 0;
    }
}

typedef struct {
    int in_range;
    uint64_t addr; /* touched start (after §8.1.6 alignment for dword+ units) */
} unit;

/* §8.1.5 address calculation and §8.1.5.3 range check for one access unit that starts
 * `delta` bytes into the instruction's access (component i of DWORDxN: delta = 4*i). */
static unit check_unit(const gcn_vsharp* v, const gcn_addr* a, uint32_t delta,
                       uint32_t touch_bytes, int dword_aligned, unsigned r) {
    const uint64_t index =
        m32((uint64_t)(a->idxen ? a->vgpr_index : 0) + (v->add_tid ? (a->lane & 63u) : 0), r);
    const uint64_t offset =
        m32((uint64_t)(a->offen ? a->vgpr_offset : 0) + a->inst_offset + delta, r);
    uint64_t buffer_offset;
    if (v->swizzle_en) {
        const uint64_t index_msb = index / v->index_stride, index_lsb = index % v->index_stride;
        const uint64_t offset_msb = offset / v->element_size;
        const uint64_t offset_lsb = offset % v->element_size;
        buffer_offset = m32((index_msb * v->stride + offset_msb * v->element_size) *
                                    v->index_stride +
                                index_lsb * v->element_size + offset_lsb,
                            r);
    } else {
        buffer_offset = m32(index * v->stride + offset, r);
    }
    const uint64_t base = v->base + a->sgpr_offset;
    const uint64_t byte_address = base + buffer_offset;
    int oob;
    if (v->stride == 0) {
        if (r & GCN_R_SOFF_SUM) {
            oob = buffer_offset + a->sgpr_offset >= v->num_records;
        } else {
            oob = buffer_offset >= (uint32_t)(v->num_records - a->sgpr_offset);
        }
    } else {
        oob = index >= v->num_records || ((a->idxen || v->add_tid) && offset >= v->stride);
    }
    unit u;
    u.addr = byte_address;
    if (dword_aligned) {
        u.addr &= ~(uint64_t)3; /* §8.1.6 */
    } else if (touch_bytes == 2 && (r & GCN_R_SHORT_ALIGN)) {
        u.addr &= ~(uint64_t)1; /* not stated by the ISA (SPEC.md) */
    }
    if (!oob && (r & GCN_R_END_WHOLE)) {
        const uint64_t end = u.addr + touch_bytes;
        /* "the end" is the end of the buffer: num_records bytes (raw) or stride * num_records. */
        const uint64_t size = v->stride == 0 ? v->num_records : (uint64_t)v->stride * v->num_records;
        oob = end > v->base + size;
    }
    u.in_range = !oob;
    return u;
}

static uint8_t* mem_at(gcn_mem* mem, uint64_t addr, uint64_t n) {
    if (addr < mem->lo || addr + n > mem->lo + mem->size || addr + n < addr) {
        return 0;
    }
    return mem->bytes + (addr - mem->lo);
}

static uint64_t read_le(const uint8_t* p, uint32_t n) {
    uint64_t v = 0;
    for (uint32_t i = 0; i < n; i++) {
        v |= (uint64_t)p[i] << (8 * i);
    }
    return v;
}

static void write_le(uint8_t* p, uint64_t v, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

static void touch(gcn_result* out, uint64_t addr, uint64_t n) {
    if (out->touch_lo == out->touch_hi) {
        out->touch_lo = addr;
        out->touch_hi = addr + n;
        return;
    }
    if (addr < out->touch_lo) out->touch_lo = addr;
    if (addr + n > out->touch_hi) out->touch_hi = addr + n;
}

static uint32_t atomic32(enum gcn_atomic op, uint32_t old, uint32_t src, uint32_t cmp) {
    switch (op) {
    case GCN_AT_SWAP: return src;
    case GCN_AT_CMPSWAP: return old == cmp ? src : old;
    case GCN_AT_ADD: return old + src;
    case GCN_AT_SUB: return old - src;
    case GCN_AT_SMIN: return (int32_t)src < (int32_t)old ? src : old;
    case GCN_AT_UMIN: return src < old ? src : old;
    case GCN_AT_SMAX: return (int32_t)src > (int32_t)old ? src : old;
    case GCN_AT_UMAX: return src > old ? src : old;
    case GCN_AT_AND: return old & src;
    case GCN_AT_OR: return old | src;
    case GCN_AT_XOR: return old ^ src;
    case GCN_AT_INC: return old >= src ? 0 : old + 1;
    case GCN_AT_DEC: return (old == 0 || old > src) ? src : old - 1;
    }
    return old;
}

static uint64_t atomic64(enum gcn_atomic op, uint64_t old, uint64_t src, uint64_t cmp) {
    switch (op) {
    case GCN_AT_SWAP: return src;
    case GCN_AT_CMPSWAP: return old == cmp ? src : old;
    case GCN_AT_INC: return old >= src ? 0 : old + 1;
    case GCN_AT_DEC: return (old == 0 || old > src) ? src : old - 1;
    case GCN_AT_ADD: return old + src;
    case GCN_AT_SUB: return old - src;
    case GCN_AT_SMIN: return (int64_t)src < (int64_t)old ? src : old;
    case GCN_AT_UMIN: return src < old ? src : old;
    case GCN_AT_SMAX: return (int64_t)src > (int64_t)old ? src : old;
    case GCN_AT_UMAX: return src > old ? src : old;
    case GCN_AT_AND: return old & src;
    case GCN_AT_OR: return old | src;
    case GCN_AT_XOR: return old ^ src;
    default: return old;
    }
}

static void scalar_load(const gcn_vsharp* v, const gcn_op* op, unsigned r, gcn_mem* mem,
                        gcn_result* out) {
    /* §7.2.2: base, stride and num_records only; address truncated to a dword. */
    for (uint32_t i = 0; i < op->ncomp && i < 16; i++) {
        const uint64_t offset = m32((uint64_t)op->smem_offset + 4ull * i, r);
        int oob;
        if (v->stride == 0) {
            oob = offset >= v->num_records;
        } else {
            const uint64_t limit = m32((uint64_t)v->stride * v->num_records, r);
            oob = (r & GCN_R_SMEM_GE) ? offset >= limit : offset > limit;
        }
        if (!oob && (r & GCN_R_END_WHOLE)) {
            /* the truncated dword must lie entirely inside */
            const uint64_t end = (offset & ~(uint64_t)3) + 4;
            oob = end > (v->stride == 0 ? v->num_records : m32((uint64_t)v->stride * v->num_records, r));
        }
        if (oob) {
            out->written[i] = (r & GCN_R_SMEM_ZERO) != 0;
            out->value[i] = 0;
            continue;
        }
        const uint64_t addr = (v->base + offset) & ~(uint64_t)3;
        const uint8_t* p = mem_at(mem, addr, 4);
        out->any_in_range = 1;
        touch(out, addr, 4);
        out->written[i] = 1;
        if (!p) {
            out->unknown = 1;
            continue;
        }
        out->value[i] = (uint32_t)read_le(p, 4);
    }
}

void gcn_execute(const gcn_vsharp* v, const gcn_addr* a, const gcn_op* op, const uint32_t data[4],
                 unsigned r, gcn_mem* mem, gcn_result* out) {
    memset(out, 0, sizeof(*out));
    if (op->kind == GCN_SLOAD) {
        scalar_load(v, op, r, mem, out);
        return;
    }
    /* §8.1.4 NOTE: resource data format INVALID means an unbound resource. */
    int unbound = v->dfmt == GCN_DFMT_INVALID;
    if (op->inst_typed && (r & GCN_R_MTBUF_BOUND)) {
        unbound = 0;
    }

    if (op->kind == GCN_ATOMIC) {
        out->written[0] = op->atomic_return;
        const unit u = check_unit(v, a, 0, op->comp_bytes, 1, r);
        if (unbound || !u.in_range) {
            return; /* dropped; returns 0 */
        }
        out->any_in_range = 1;
        touch(out, u.addr, op->comp_bytes);
        uint8_t* p = mem_at(mem, u.addr, op->comp_bytes);
        if (!p) {
            out->unknown = 1;
            return;
        }
        if (op->comp_bytes == 8) {
            const uint64_t old = read_le(p, 8);
            /* X2: DATA[0:1] is the source, DATA[2:3] the CMPSWAP compare value */
            const uint64_t src = data[0] | ((uint64_t)data[1] << 32);
            const uint64_t cmp = data[2] | ((uint64_t)data[3] << 32);
            write_le(p, atomic64(op->atomic, old, src, cmp), 8);
            out->value[0] = (uint32_t)old;
            out->value[1] = (uint32_t)(old >> 32);
        } else {
            const uint32_t old = (uint32_t)read_le(p, 4);
            write_le(p, atomic32(op->atomic, old, data[0], data[1]), 4);
            out->value[0] = old;
        }
        return;
    }

    if (op->typed && unbound) {
        /* Unbound resource: (0,0,0,0) for loads (Table 8.5 footnote), stores dropped. */
        return;
    }
    if (op->typed) {
        const uint32_t dfmt = op->inst_typed ? op->inst_dfmt : v->dfmt;
        const uint32_t nfmt = op->inst_typed ? op->inst_nfmt : v->nfmt;
        uint32_t k = 0, cb = 0, elem = 0, shift[4], width[4];
        if (!gcn_format_bits(dfmt, &elem, &k, shift, width)) {
            out->unknown = 1;
            return;
        }
        const int byte_comps = gcn_format_layout(dfmt, &k, &cb);
        if (op->kind == GCN_STORE && (!byte_comps || (nfmt != GCN_NFMT_UINT && nfmt != GCN_NFMT_SINT &&
                                                      !(nfmt == GCN_NFMT_FLOAT && cb == 4)))) {
            out->unknown = 1; /* typed stores model only integer and 32-bit float formats */
            return;
        }
        /* A whole element of 4 bytes or more is a "Dword or larger" access (§8.1.6). */
        const unit u = check_unit(v, a, 0, elem, elem >= 4, r);
        const int in_range = !unbound && u.in_range;
        uint32_t comps[4] = {0, 0, 0, 0};
        if (in_range) {
            out->any_in_range = 1;
            touch(out, u.addr, elem);
            uint8_t* p = mem_at(mem, u.addr, elem);
            if (!p) {
                out->unknown = 1;
                return;
            }
            if (op->kind == GCN_STORE) {
                for (uint32_t c = 0; c < k; c++) {
                    write_le(p + c * cb, data[c], cb);
                }
                return;
            }
            const uint64_t lo = read_le(p, elem < 8 ? elem : 8);
            for (uint32_t c = 0; c < k; c++) {
                uint32_t raw;
                if (byte_comps) {
                    raw = (uint32_t)read_le(p + c * cb, cb);
                } else {
                    const uint32_t sh = (r & GCN_R_PACK_MIRROR) ? 32 - shift[c] - width[c] : shift[c];
                    raw = (uint32_t)((lo >> sh) & ((1ull << width[c]) - 1));
                }
                int unk = 0;
                comps[c] = gcn_convert_component(raw, width[c], nfmt, &unk);
                if (unk) out->unknown = 1;
            }
        } else if (op->kind == GCN_STORE) {
            return;
        }
        /* Table 8.4: TBUFFER uses identity dst_sel (X000, XY00, XYZ0, XYZW); BUFFER_*_FORMAT the
         * resource's. Constant ONE is 1 in the number format. */
        uint8_t sel[4];
        for (uint32_t c = 0; c < 4; c++) {
            sel[c] = op->inst_typed ? (uint8_t)(c < k ? 4 + c : 0) : v->dst_sel[c];
        }
        /* "integer formats are returned as integers" (§8.1.3): integer 1 for UINT/SINT, 1.0f for
         * every other number format. (Decided; SEL1_FLOAT is no longer an accepted reading.) */
        const int int_fmt = nfmt == GCN_NFMT_UINT || nfmt == GCN_NFMT_SINT;
        const uint32_t one = !int_fmt ? 0x3f800000u : 1u;
        for (uint32_t c = 0; c < op->ncomp && c < 4; c++) {
            if (!in_range && !(r & GCN_R_FMT_SEL)) {
                out->value[c] = 0;
            } else if (sel[c] == 1) {
                out->value[c] = one;
            } else if (sel[c] >= 4 && sel[c] - 4u < k) {
                out->value[c] = comps[sel[c] - 4];
            } else {
                out->value[c] = 0;
            }
        }
        return;
    }

    /* Raw: per-component range check (§8.1.5.3 "per component"). */
    for (uint32_t i = 0; i < op->ncomp && i < 4; i++) {
        const unit u = check_unit(v, a, i * op->comp_bytes, op->comp_bytes, op->comp_bytes >= 4, r);
        if (unbound || !u.in_range) {
            out->value[i] = 0;
            continue;
        }
        out->any_in_range = 1;
        touch(out, u.addr, op->comp_bytes);
        uint8_t* p = mem_at(mem, u.addr, op->comp_bytes);
        if (!p) {
            out->unknown = 1;
            continue;
        }
        if (op->kind == GCN_STORE) {
            write_le(p, data[i], op->comp_bytes);
            continue;
        }
        uint64_t value = read_le(p, op->comp_bytes);
        if (op->sign_extend && op->comp_bytes < 4) {
            const uint32_t bits = op->comp_bytes * 8;
            if (value & (1ull << (bits - 1))) {
                value |= ~0ull << bits;
            }
        }
        out->value[i] = (uint32_t)value;
    }
}

uint32_t gcn_popcount64(uint64_t x) {
    uint32_t n = 0;
    for (; x; x &= x - 1) n++;
    return n;
}

uint32_t gcn_mbcnt(uint32_t s0, uint32_t s1, uint32_t lane, int hi) {
    const uint64_t thread_mask = lane >= 64 ? ~0ull : (1ull << lane) - 1;
    const uint32_t half = hi ? (uint32_t)(thread_mask >> 32) : (uint32_t)thread_mask;
    return gcn_popcount64(s0 & half) + s1;
}

uint32_t gcn_first_lane(uint64_t exec) {
    for (uint32_t lane = 0; lane < 64; lane++) {
        if (exec & (1ull << lane)) return lane;
    }
    return 0;
}

uint16_t gcn_f32_to_f16_rtz(uint32_t bits, int flush_out) {
    const uint16_t sign = (uint16_t)((bits >> 16) & 0x8000u);
    const int32_t exp = (int32_t)((bits >> 23) & 0xFFu);
    const uint32_t frac = bits & 0x7FFFFFu;
    if (exp == 0xFF) return (uint16_t)(sign | (frac ? 0x7E00u : 0x7C00u));
    const int32_t e = exp - 127; /* unbiased; f32 denormals are far below the f16 range */
    if (exp == 0) return sign;
    if (e > 15) return (uint16_t)(sign | 0x7BFFu); /* toward zero: the largest finite half */
    if (e >= -14) return (uint16_t)(sign | ((uint32_t)(e + 15) << 10) | (frac >> 13));
    /* below the normal range: value / 2^-24, truncated */
    const int32_t shift = -1 - e; /* (1.frac * 2^e) / 2^-24 = (0x800000 | frac) >> shift */
    const uint32_t m = shift >= 32 ? 0u : ((0x800000u | frac) >> shift);
    if (flush_out && m != 0) return sign;
    return (uint16_t)(sign | m);
}
