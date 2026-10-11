/* GCN2 (Sea Islands) buffer access reference model. Plain C, written from the ISA text only
 * (see SPEC.md); it shares no code with the shader recompiler. */
#ifndef GCN_REF_H
#define GCN_REF_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Ambiguity readings (SPEC.md table). Reading 0 is the literal text. */
enum {
    GCN_R_WRAP32 = 1 << 0,
    GCN_R_SOFF_SUM = 1 << 1,
    GCN_R_END_WHOLE = 1 << 2,
    GCN_R_SMEM_GE = 1 << 3,
    GCN_R_SMEM_ZERO = 1 << 4,
    GCN_R_FMT_SEL = 1 << 5,
    GCN_R_MTBUF_BOUND = 1 << 6,
    GCN_R_SHORT_ALIGN = 1 << 7,
    GCN_R_SEL1_FLOAT = 1 << 8,
    GCN_R_PACK_MIRROR = 1 << 9, /* packed formats laid out from the MSB (X in the top bits) */
    GCN_NUM_READINGS = 1 << 10,
};

typedef struct {
    uint64_t base; /* const_base */
    uint32_t stride;
    uint32_t num_records;
    int swizzle_en;
    uint32_t index_stride; /* 8, 16, 32, 64 */
    uint32_t element_size; /* 2, 4, 8, 16 */
    int add_tid;
    uint32_t dfmt, nfmt;   /* resource data/number format */
    uint8_t dst_sel[4];    /* 0=0 1=1 4=X 5=Y 6=Z 7=W */
} gcn_vsharp;

typedef struct {
    int idxen, offen;
    uint32_t vgpr_index, vgpr_offset, inst_offset, sgpr_offset, lane;
} gcn_addr;

enum gcn_kind { GCN_LOAD, GCN_STORE, GCN_ATOMIC, GCN_SLOAD };
enum gcn_atomic {
    GCN_AT_SWAP, GCN_AT_CMPSWAP, GCN_AT_ADD, GCN_AT_SUB, GCN_AT_SMIN, GCN_AT_UMIN, GCN_AT_SMAX,
    GCN_AT_UMAX, GCN_AT_AND, GCN_AT_OR, GCN_AT_XOR, GCN_AT_INC, GCN_AT_DEC
};

typedef struct {
    enum gcn_kind kind;
    uint32_t comp_bytes;  /* raw: 1, 2 or 4 (8 for 64-bit atomics) */
    uint32_t ncomp;       /* raw dwordxN count / format instruction component count */
    int sign_extend;      /* sbyte / sshort */
    int typed;            /* BUFFER_*_FORMAT (resource format) or TBUFFER_* */
    int inst_typed;       /* TBUFFER_*: format from the instruction, identity dst_sel */
    uint32_t inst_dfmt, inst_nfmt;
    enum gcn_atomic atomic;
    int atomic_return;    /* glc */
    uint32_t smem_offset; /* GCN_SLOAD: byte offset (imm*4 or SGPR value) */
} gcn_op;

/* Model of guest memory: bytes for [lo, lo+size). */
typedef struct {
    uint64_t lo;
    uint64_t size;
    uint8_t* bytes;
} gcn_mem;

typedef struct {
    uint32_t value[16];   /* VGPR (or SGPR for GCN_SLOAD, up to 16 dwords) results */
    int written[16];      /* scalar loads: whether SDST i was written */
    int unknown;          /* touched memory outside the model: expected value unknowable */
    uint64_t touch_lo, touch_hi; /* union of bytes the access reads or writes; lo==hi: none */
    int any_in_range;
} gcn_result;

/* Data formats (Table in §8, same numbering as the V# data_format field). */
enum {
    GCN_DFMT_INVALID = 0, GCN_DFMT_8 = 1, GCN_DFMT_16 = 2, GCN_DFMT_8_8 = 3, GCN_DFMT_32 = 4,
    GCN_DFMT_16_16 = 5, GCN_DFMT_10_11_11 = 6, GCN_DFMT_11_11_10 = 7, GCN_DFMT_10_10_10_2 = 8,
    GCN_DFMT_2_10_10_10 = 9, GCN_DFMT_8_8_8_8 = 10, GCN_DFMT_32_32 = 11, GCN_DFMT_16_16_16_16 = 12,
    GCN_DFMT_32_32_32 = 13, GCN_DFMT_32_32_32_32 = 14
};
enum { GCN_NFMT_UNORM = 0, GCN_NFMT_SNORM = 1, GCN_NFMT_USCALED = 2, GCN_NFMT_SSCALED = 3,
       GCN_NFMT_UINT = 4, GCN_NFMT_SINT = 5, GCN_NFMT_SNORM_OGL = 6, GCN_NFMT_FLOAT = 7 };

/* Component layout of a buffer data format: element size and each component's bit offset and
 * width (packed formats are named MSB first: 2_10_10_10 has X in bits 9:0, W in 31:30). */
int gcn_format_bits(uint32_t dfmt, uint32_t* elem_bytes, uint32_t* ncomp, uint32_t shift[4], uint32_t width[4]);
/* A component's register value for a number format (VGPR bits): UNORM x/(2^n-1), SNORM
 * max(x/(2^(n-1)-1), -1), SCALED float(x), UINT/SINT the extended integer, FLOAT 32/16/11/10-bit
 * floats as f32. Returns 0 for a combination the model does not know (sets *unknown). */
uint32_t gcn_convert_component(uint32_t raw, uint32_t width, uint32_t nfmt, int* unknown);

/* Returns 0 when the format is outside what the model supports. */
int gcn_format_layout(uint32_t dfmt, uint32_t* ncomp, uint32_t* comp_bytes);

/* Executes one lane's access against `mem` under `reading`; updates mem for stores/atomics.
 * `data` is the VGPR data (stores/atomics; data[1] is the CMPSWAP compare value). */
void gcn_execute(const gcn_vsharp* v, const gcn_addr* a, const gcn_op* op, const uint32_t data[4],
                 unsigned reading, gcn_mem* mem, gcn_result* out);

/* Wave64 lane semantics (Sea Islands ISA chapter 12 instruction descriptions). */
/* V_MBCNT_LO/HI_U32_B32: ThreadMask = (1 << ThreadPosition) - 1;
 * D = CountOneBits(S0 & ThreadMask[31:0] or [63:32]) + S1. */
uint32_t gcn_mbcnt(uint32_t s0, uint32_t s1, uint32_t lane, int hi);
/* V_READFIRSTLANE_B32: Lane# = FindFirst1fromLSB(exec), lane 0 if exec is zero. */
uint32_t gcn_first_lane(uint64_t exec);
uint32_t gcn_popcount64(uint64_t x);
/* V_CVT_PKRTZ_F16_F32 component: flt32_to_flt16 with round-toward-zero (ISA ch. 12). The ISA
 * does not say how denormal f16 results or NaN payloads come out: `flush_out` selects the
 * flushed reading; NaN inputs return 0x7E00 | sign and callers accept any NaN. */
uint16_t gcn_f32_to_f16_rtz(uint32_t bits, int flush_out);

/* f32 -> unsigned small float (5-bit exponent, bias 15, `mant_bits` 6 or 5: the 11/10-bit fields
 * of 10_11_11 / 11_11_10). Table 8.13 gives no conversion rules; the readings name each choice
 * (0 = the EXT_packed_float/Mesa convention): */
enum {
    GCN_UF_RTZ = 1 << 0,     /* round toward zero (else to nearest, ties to even) */
    GCN_UF_INF = 1 << 1,     /* finite overflow gives +Inf (else the largest finite value) */
    GCN_UF_FLUSH = 1 << 2,   /* results below 2^-14 flush to 0 (else denormals kept) */
    GCN_UF_NEGNAN0 = 1 << 3, /* a negative NaN gives 0 (else a NaN) */
    GCN_UF_READINGS = 1 << 4,
};
#define GCN_UF_ANY_NAN 0xFFFFFFFFu /* any NaN encoding (exponent all ones, mantissa nonzero) */
/* Negative values, -0 and -Inf give 0; +Inf gives Inf; f32 denormals give 0. */
uint32_t gcn_f32_to_ufloat(uint32_t bits, uint32_t mant_bits, unsigned readings);

#ifdef __cplusplus
}
#endif
#endif
