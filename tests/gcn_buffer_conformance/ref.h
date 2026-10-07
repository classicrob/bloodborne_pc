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
    GCN_NUM_READINGS = 1 << 9,
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
    uint32_t value[4];    /* VGPR (or SGPR for GCN_SLOAD) results */
    int written[4];       /* scalar loads: whether SDST i was written */
    int unknown;          /* touched memory outside the model: expected value unknowable */
    uint64_t touch_lo, touch_hi; /* union of bytes the access reads or writes; lo==hi: none */
    int any_in_range;
} gcn_result;

/* Data formats (Table in §8, same numbering as the V# data_format field). */
enum {
    GCN_DFMT_INVALID = 0, GCN_DFMT_8 = 1, GCN_DFMT_16 = 2, GCN_DFMT_8_8 = 3, GCN_DFMT_32 = 4,
    GCN_DFMT_16_16 = 5, GCN_DFMT_8_8_8_8 = 10, GCN_DFMT_32_32 = 11, GCN_DFMT_16_16_16_16 = 12,
    GCN_DFMT_32_32_32 = 13, GCN_DFMT_32_32_32_32 = 14
};
enum { GCN_NFMT_UINT = 4, GCN_NFMT_FLOAT = 7 };

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

#ifdef __cplusplus
}
#endif
#endif
