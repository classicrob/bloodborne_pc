// SPDX-License-Identifier: GPL-2.0-or-later
// GCN2 buffer conformance suite: encoded GCN shaders -> production shader recompiler
// (TranslateProgram + EmitSPIRV + StageSpecialization permutation reuse) -> MoltenVK, with a
// binder mirroring Rasterizer::BindBuffers. Results are compared against ref.c (written from the
// Sea Islands ISA; see SPEC.md and README.md).
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cinttypes>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/backend/spirv/emit_spirv_quad_rect.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/runtime_info.h"
#include "shader_recompiler/specialization.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_common.h"

#include "gcn_asm.h"
#include "shader_recompiler/frontend/decode.h"
#include <magic_enum/magic_enum.hpp>
#include "ref.h"

VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

using u8 = uint8_t;
using u32 = uint32_t;
using u64 = uint64_t;

namespace {

// ---------------------------------------------------------------------------------------------
// Guest address space of the harness. Every buffer the shaders see is a guest V#; the binder
// maps guest addresses into these VkBuffers like the buffer cache maps guest memory.
constexpr u64 GuestVa = 0x2000000000ull;  // arenas under test
constexpr u64 ParamVa = 0x3000000000ull;  // per-lane parameters
constexpr u64 ResultVa = 0x3800000000ull; // per-lane results
constexpr u32 Arena = 64 * 1024;          // per-job slot of patterned guest memory
constexpr u32 BaseInArena = 16 * 1024;    // V# base offset inside its arena (before misalign)
constexpr u32 kLanes = 128; // up to two GCN waves per workgroup
constexpr u32 ParamSlot = kLanes * 32;
constexpr u32 ResultSlot = kLanes * 16;
constexpr u32 FlatSlot = 4096;
constexpr u32 MaxBatch = 256;
constexpr u32 VgprMarker = 0xDEAD0000u;
constexpr u32 SgprMarker = 0x5EED0000u;
constexpr u32 ResultFill = 0x0BAD0BADu;
constexpr u32 NullBufferSize = 64; // vk_null_resources.h NullResources::BufferSize
constexpr u32 RenderSize = 16;     // render group target: 16x16 R32G32B32A32_SFLOAT
constexpr u32 RenderBytes = RenderSize * RenderSize * 16;

u32 Hash32(u64 x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ull;
    x ^= x >> 33;
    return u32(x);
}
// Nonzero bytes everywhere, so a zero result never coincides with memory contents.
u8 BufferByte(u64 va) { return u8(Hash32(va) % 255 + 1); }
u8 CanaryByte(u64 va) { return u8(Hash32(va ^ 0xC0FFEE123ull) % 255 + 1); }

// ---------------------------------------------------------------------------------------------
enum class SoffSrc { Inline, Sgpr };
enum class SmemSrc { None, Imm, Sgpr };
// Wave64 lane-semantics programs (no buffer access under test).
enum class Wave { None, Lane, MbcntMask, Readlane, Exec1, Exec2, LoopReadFirstLane, Lds, CtrlPhi };

struct Template {
    std::string name;
    gcn_op op{};
    bool mtbuf = false;
    u32 opcode = 0;
    bool idxen = false, offen = false, glc = false;
    u32 inst_offset = 0;
    SoffSrc soff = SoffSrc::Inline;
    u32 soff_inline = 0;
    SmemSrc smem = SmemSrc::None;
    u32 smem_imm = 0;      // dwords
    std::array<u32, 4> smem_pick{0, 1, 2, 3}; // which SDST dwords are copied to v10..v13
    bool combo = false;    // s_buffer_load_dword (imm) + buffer_load_dword on the same V#
    Wave wave = Wave::None;
    bool vertex = false;   // vertex shader + fetch shader (s_swappc_b64), indexed draw
    bool indirect = false; // vertex: indirect draw; the fetch shader adds the base vertex from s13
    bool fragment = false; // fragment stage (compile-only)
    bool render = false;   // vertex: rasterized draw into a 16x16 target, read back
    bool rect_list = false; // render: RectList through the production tessellation emulation
    bool vs_fetch = false;  // render: positions from a fetch shader (else from the vertex id)
    u32 pos_scale = 246;    // render, vertex-id positions: inline float (246 = 4.0, 244 = 2.0, 242 = 1.0)
    u32 fs_tmpl = ~0u;      // render: the fragment shader template
    bool helper = false;    // compiled for another job; no verdict of its own
    bool expect_loud = false; // compile-only: the recompiler must reject this shader
    bool compute_compile = false; // compile-only compute template (normal assembly, not run)
    bool force_int64_atomics = false; // compile with Profile::supports_buffer_int64_atomics forced on
    bool compile_only = false; // translate + EmitSPIRV + spirv-val only; an abort is a FAIL
    bool srt = false;      // V# and a constant read through an extended-user-data pointer (s[2:3])
    u32 sdst = 16;         // stage-compile: SDST of the s_buffer_load (0 overlaps user data)
    bool dead = false;     // the access under test is dead (its result is never used)
    bool wave_divergent = false; // wave: cross-lane ops in divergent flow (loud by design on Apple)
    bool clip_planes = false;  // vertex: user clip plane 0 enabled (adds the ClipPlanes buffer)
    bool check_bindings = false; // compile-only: the chosen module's bindings must start at the job's start
    bool no_buffer_load = false; // stage-compile: export constants only (no s_buffer_load)
    std::vector<u32> fetch;
    std::array<u32, 3> wg{64, 1, 1}; // compute workgroup shape (power-of-two x)
    u32 lds_bits = 0;                // Wave::Lds: guest LDS access width (16, 32, 64)
    u32 Threads() const { return wg[0] * wg[1] * wg[2]; }
    std::vector<u32> code;
    u64 hash = 0;
};

struct Lane {
    u32 active = 0, vindex = 0, voffset = 0;
    u32 data[4]{};
};

struct VertexAttr {
    u32 semantic = 0, format = 0; // vk::Format from LiverpoolToVK::SurfaceFormat
    std::array<u32, 4> sharp{};
};

struct BindPlan {
    u32 type = 0; // Shader::BufferType
    u64 required = 0; // BufferResource::required_binding_bytes, when the tree has it
    std::array<u32, 4> sharp{};
    std::vector<u32> flat;
};

enum class Status { Pending, Ok, LoudCompile, Crash, HarnessError, LoudBinder, LoudPipeline };

struct Job {
    u32 tmpl = 0;
    std::string group, label;
    gcn_vsharp v{};
    u64 base_delta = 0; // V# base = slot base + BaseInArena + base_delta (unless zero_sharp)
    bool zero_sharp = false;
    u32 sgpr_soff = 0, smem_sgpr = 0;
    std::vector<u32> init_words; // written at the V# base after the pattern fill
    bool quarter = false;        // render: only the top-left 8x8 quarter is covered
    bool force_push = false;     // moltenvk-push: bind with push descriptors whatever the default
    s32 vertex_offset = 0; // vertex jobs: drawIndexed vertexOffset (lanes hold index + offset)
    u32 start_unified = 0, start_buffer = 0; // Backend::Bindings at which this stage starts
    std::array<Lane, kLanes> lanes{};
    bool approved_struct_soff = false, approved_swizzle = false;
    bool isa_invalid = false; // violates an ISA programming rule (e.g. element_size < access size)
    bool approved_offen_only = false; // structured, offen without idxen/add_tid: offset never checked
    u32 slot = 0;
    std::array<u32, 16> ud{};
    // compile output
    Status status = Status::Pending;
    std::string message;
    u32 perm = 0;
    std::vector<BindPlan> binds;
    std::array<u32, 16> ud_regs{};
    std::vector<VertexAttr> vattrs; // fixed-function vertex inputs of the permutation
    std::array<u32, 8> vtable{};    // vertex/srt jobs: table at s[2:3] (V# in dwords 0..3, constant in 4)
};

std::vector<Template> g_templates;
std::vector<Job> g_jobs;

std::array<u32, 4> Pack(const gcn_vsharp& v) {
    auto code = [](u32 value, u32 first) {
        u32 c = 0;
        while ((first << c) < value) ++c;
        return c & 3;
    };
    std::array<u32, 4> d{};
    d[0] = u32(v.base);
    d[1] = u32(v.base >> 32) & 0xFF;
    d[1] |= (v.stride & 0x3FFF) << 16;
    d[1] |= v.swizzle_en ? 1u << 31 : 0;
    d[2] = v.num_records;
    d[3] = (v.dst_sel[0] & 7) | (v.dst_sel[1] & 7) << 3 | (v.dst_sel[2] & 7) << 6 |
           (v.dst_sel[3] & 7) << 9 | (v.nfmt & 7) << 12 | (v.dfmt & 15) << 15 |
           code(v.element_size ? v.element_size : 2, 2) << 19 |
           code(v.index_stride ? v.index_stride : 8, 8) << 21 | (v.add_tid ? 1u << 23 : 0);
    return d;
}

u64 BoundSize(const gcn_vsharp& v) { // AmdGpu::Buffer::GetSize(): u32 arithmetic
    return v.stride == 0 ? v.num_records : u32(v.stride * v.num_records);
}

// ---------------------------------------------------------------------------------------------
// Shader templates

// v2 = active (exec predicate), v3 = per-lane value, v5 = ballot predicate, v9 = 0; s12/s13 = lane selects.
void AssembleWave(GcnAsm::Code& c, Wave w) {
    using namespace GcnAsm;
    switch (w) {
    case Wave::Lane: // lane id via mbcnt(-1), ballot via VOPC -> VCC, s_bcnt1 of the ballot
        c.VMbcnt(false, 10, InlineNeg1, 9);
        c.VMbcnt(true, 10, InlineNeg1, 10);
        c.VCmpNeU32(Inline(0), 5);
        c.VMov(11, VCC_LO);
        c.VMov(12, VCC_HI);
        c.SBcnt1B64(16, VCC_LO);
        c.VMov(13, 16);
        break;
    case Wave::MbcntMask: // compaction index: mbcnt over the ballot; s_mov_b64 of VCC; v_cndmask on VCC
        c.VCmpNeU32(Inline(0), 5);
        c.VMbcnt(false, 10, VCC_LO, 9);
        c.VMbcnt(true, 10, VCC_HI, 10);
        c.SMovB64(16, VCC_LO);
        c.VMov(11, 16);
        c.VMov(12, 17);
        c.VCndmask(13, Inline(0), 2); // v2 == 1 in every lane of these jobs
        break;
    case Wave::Readlane: // v_readlane across the 64 lanes; readfirstlane with a full exec
        c.Sop1(3, 17, Inline(37));
        c.VReadlane(16, 3, 17);
        c.VReadlane(18, 3, 13);
        c.VReadlane(19, 3, 12);
        c.VReadfirstlane(20, 3);
        c.VMov(10, 16);
        c.VMov(11, 18);
        c.VMov(12, 19);
        c.VMov(13, 20);
        break;
    case Wave::Exec1: // scalar results computed under a partial exec are wave-uniform
        c.VCmpNeU32(Inline(0), 2);
        c.SAndSaveexecB64(22, VCC_LO);
        c.VReadfirstlane(21, 3);
        c.SBcnt1B64(24, EXEC_LO);
        c.VMov(13, Vgpr(3));
        c.SMovB64(EXEC_LO, 22);
        c.VMov(10, 21);
        c.VMov(11, 24);
        break;
    case Wave::Exec2: // exec as a 64-bit value; mbcnt over exec inside the scope
        c.VCmpNeU32(Inline(0), 2);
        c.SAndSaveexecB64(22, VCC_LO);
        c.SMovB64(26, EXEC_LO);
        c.VMbcnt(false, 12, 26, 9);
        c.VMbcnt(true, 12, 27, 12);
        c.SMovB64(EXEC_LO, 22);
        c.VMov(10, 26);
        c.VMov(11, 27);
        break;
    case Wave::LoopReadFirstLane: {
        // s21 = v_readfirstlane(v3) (one value per wave); loop: ballot of v5 != 0 counted into s24,
        // s20 += s21 until s20 >= 0x3000. The loop exit is wave-uniform, workgroup-uniform only
        // for a single-wave workgroup (loop-carried through the s20 phi).
        c.Sop1(3, 20, Inline(0));
        c.Sop1(3, 24, Inline(0));
        c.VReadfirstlane(21, 3);
        c.Sop2(14, 21, 21, LITERAL); // s_and_b32 s21, s21, 0xF000 | 0x1000 floor: never 0 (bounded loop)
        c.words.push_back(0xF000u);
        c.Sop2(16, 21, 21, LITERAL); // s_or_b32 s21, s21, 0x1000
        c.words.push_back(0x1000u);
        const size_t loop = c.words.size();
        c.VCmpNeU32(Inline(0), 5);
        c.SBcnt1B64(22, VCC_LO);
        c.SAddU32(24, 24, 22);
        c.SAddU32(20, 20, 21);
        c.SCmpLtU32Literal(20, 0x3000);
        c.SCbranchScc1To(loop);
        c.VMov(10, 24);
        c.VMov(11, 20);
        break;
    }
    case Wave::None:
    case Wave::Lds:
        break;
    }
}

// Guest LDS of one width (each lane writes and reads its own slot) next to a VOPC ballot.
void AssembleLds(GcnAsm::Code& c, u32 bits) {
    using namespace GcnAsm;
    const u32 shift = bits == 64 ? 3 : (bits == 32 ? 2 : 1);
    c.VLshlrevB32(16, Inline(shift), 15);
    c.Ds(bits == 64 ? 77 : (bits == 32 ? 13 : 31), 16, 3, 0); // ds_write v16, v3 (v[3:4] for b64)
    c.SWaitcntAll();
    c.SBarrier();
    c.Ds(bits == 64 ? 118 : (bits == 32 ? 54 : 60), 16, 0, 10); // ds_read -> v10 (v[10:11] for b64)
    c.SWaitcntAll();
    c.VCmpNeU32(Inline(0), 5);
    c.VMov(12, VCC_LO);
    c.VMov(13, VCC_HI);
}

constexpr u32 IndirectBaseSgpr = 13;

// Vertex shader: s_swappc_b64 into the fetch shader (V# via s_load_dwordx4 from the table at
// s[2:3], buffer/tbuffer_load_format_xyzw v[4:7], v0 idxen), then stores v[4:7] to the result
// slot (vertex index & 63) and exports a position. Rasterization is discarded.
void AssembleVertex(Template& t) {
    using namespace GcnAsm;
    Code c;
    c.SMovLiteral(VCC_HI, 0x5eed);
    c.SSwappcB64(0, 0);
    c.Vop2(27, 9, 0, Inline(63)); // v_and_b32 v9, 63, v0
    c.VLshlrevB32(8, Inline(4), 9);
    c.Mubuf(BUFFER_STORE_DWORDX4, Code::BufFields{.offen = true, .vaddr = 8, .vdata = 4, .srsrc = 8});
    c.SWaitcntAll();
    c.VMov(10, Inline(0));
    c.VMovLiteral(11, 0x3f800000);
    c.Exp(12, 0xF, true, 10, 10, 10, 11);
    c.SEndpgm();
    Code f;
    if (t.indirect) {
        f.Vop2(37, 0, 0, IndirectBaseSgpr); // v_add_i32 v0, s13, v0: base vertex written by the CP
    }
    f.SLoadDwordx4(4, 2, 0);
    f.SWaitcntAll();
    const Code::BufFields fl{.idxen = true, .vaddr = 0, .vdata = 4, .srsrc = 4};
    if (t.mtbuf) {
        f.Mtbuf(TBUFFER_LOAD_FORMAT_XYZW, t.op.inst_dfmt, t.op.inst_nfmt, fl);
    } else {
        f.Mubuf(BUFFER_LOAD_FORMAT_XYZW, fl);
    }
    f.SWaitcntAll();
    f.SSetpcB64(0);
    t.code = std::move(c.words);
    t.fetch = std::move(f.words);
    u64 h = 0xcbf29ce484222325ull;
    for (u32 w : t.code) h = (h ^ w) * 0x100000001b3ull;
    for (u32 w : t.fetch) h = (h ^ w) * 0x100000001b3ull;
    t.hash = h;
}

// Graphics stages whose only buffer access is s_buffer_load (V# in s[4:7]), compile-only. The
// fragment shader mirrors the shape of a real game shader that broke the Apple bounds path.
// Control-dependent uniformity (compile-only, never executed: a wrong lowering would deadlock):
// s21 = readfirstlane(LII) (per wave); bound = s21 >= 64 ? 2 : 1 through a branch and a phi of two
// uniform constants; a loop up to the bound with a ballot inside. One wave: uniform; two waves:
// the trip count differs per wave, so the ballot's barriers must be refused.
void AssembleCtrlPhi(Template& t) {
    using namespace GcnAsm;
    Code c;
    c.SMovLiteral(VCC_HI, 0x5eed);
    c.VReadfirstlane(21, 0);
    c.words.push_back(0xBF000000u | (9u << 16) | (Inline(64) << 8) | 21); // s_cmp_ge_u32 s21, 64
    const size_t br_else = c.words.size();
    c.Sopp(5, 0);                     // s_cbranch_scc1 ELSE (patched)
    c.Sop1(3, 22, Inline(1));         // s_mov_b32 s22, 1
    const size_t br_join = c.words.size();
    c.Sopp(2, 0);                     // s_branch JOIN (patched)
    const size_t else_at = c.words.size();
    c.Sop1(3, 22, Inline(2));         // ELSE: s_mov_b32 s22, 2
    const size_t join_at = c.words.size();
    c.words[br_else] |= u32(int32_t(else_at) - int32_t(br_else + 1)) & 0xFFFF;
    c.words[br_join] |= u32(int32_t(join_at) - int32_t(br_join + 1)) & 0xFFFF;
    c.Sop1(3, 20, Inline(0));
    c.Sop1(3, 24, Inline(0));
    const size_t loop = c.words.size();
    c.VCmpNeU32(Inline(0), 0);
    c.SBcnt1B64(23, VCC_LO);
    c.SAddU32(24, 24, 23);
    c.SAddU32(20, 20, Inline(1));
    c.words.push_back(0xBF000000u | (10u << 16) | (22u << 8) | 20); // s_cmp_lt_u32 s20, s22
    c.SCbranchScc1To(loop);
    c.VMov(1, 24);
    c.VLshlrevB32(2, Inline(2), 0);
    c.Mubuf(BUFFER_STORE_DWORD, Code::BufFields{.offen = true, .vaddr = 2, .vdata = 1, .srsrc = 4});
    c.SEndpgm();
    t.code = std::move(c.words);
    t.hash = 0xc791ull ^ (u64(t.wg[0]) << 16);
}

void AssembleStage(Template& t) {
    using namespace GcnAsm;
    Code c;
    c.SMovLiteral(VCC_HI, 0x5eed);
    c.Sop1(3, 124, 8); // s_mov_b32 m0, s8
    if (!t.no_buffer_load) {
        c.SBufferLoad(t.op.ncomp, t.sdst, 4, t.smem == SmemSrc::Imm, t.smem == SmemSrc::Imm ? t.smem_imm : 9);
        c.SWaitcntAll();
    }
    for (u32 i = 0; i < 4; ++i) {
        if (t.dead || t.no_buffer_load) c.VMovLiteral(i, 0x3f800000);
        else c.VMov(i, t.sdst + (i < t.op.ncomp ? i : 0));
    }
    if (t.fragment) {
        c.Exp(0, 0xF, true, 0, 1, 2, 3, true); // mrt0
    } else {
        c.Exp(12, 0xF, true, 0, 1, 2, 3); // pos0
    }
    c.SEndpgm();
    t.code = std::move(c.words);
    u64 h = 0xcbf29ce484222325ull ^ (t.fragment ? 1 : 2);
    for (u32 w : t.code) h = (h ^ w) * 0x100000001b3ull;
    t.hash = h;
}

// Render group: VS positions from the vertex id (x = (id & 1) * k - 1, y = (id >> 1) * k - 1)
// or from a fetch shader (buffer_load_format_xyzw v[4:7], v0 idxen); FS exports a constant color.
constexpr float RenderColor[4] = {0.25f, 0.5f, 0.75f, 1.0f};
constexpr float ClearColor[4] = {0.125f, 0.125f, 0.125f, 0.125f};

void AssembleRender(Template& t) {
    using namespace GcnAsm;
    Code c;
    c.SMovLiteral(VCC_HI, 0x5eed);
    if (t.vs_fetch) {
        c.SSwappcB64(0, 0);
        c.Exp(12, 0xF, true, 4, 5, 6, 7);
        Code f;
        f.SLoadDwordx4(4, 2, 0);
        f.SWaitcntAll();
        f.Mubuf(BUFFER_LOAD_FORMAT_XYZW, Code::BufFields{.idxen = true, .vaddr = 0, .vdata = 4, .srsrc = 4});
        f.SWaitcntAll();
        f.SSetpcB64(0);
        t.fetch = std::move(f.words);
    } else {
        c.Vop2(27, 1, 0, Inline(1));  // v_and_b32 v1, 1, v0
        c.Vop2(22, 2, 0, Inline(1));  // v_lshrrev_b32 v2, 1, v0
        c.Vop1(6, 1, Vgpr(1));        // v_cvt_f32_u32
        c.Vop1(6, 2, Vgpr(2));
        c.Vop2(8, 1, 1, t.pos_scale); // v_mul_f32 v1, k, v1
        c.Vop2(8, 2, 2, t.pos_scale);
        c.Vop2(5, 1, 1, 242);         // v_subrev_f32 v1, 1.0, v1 -> v1 - 1
        c.Vop2(5, 2, 2, 242);
        c.VMov(3, Inline(0));
        c.VMov(4, 242);
        c.Exp(12, 0xF, true, 1, 2, 3, 4);
    }
    c.SEndpgm();
    t.code = std::move(c.words);
    u64 h = 0xcbf29ce484222325ull ^ 0x7e9d;
    for (u32 w : t.code) h = (h ^ w) * 0x100000001b3ull;
    for (u32 w : t.fetch) h = (h ^ w) * 0x100000001b3ull;
    h ^= t.rect_list ? 0x55 : 0;
    t.hash = h;
}

void AssembleColorFs(Template& t) {
    using namespace GcnAsm;
    Code c;
    c.SMovLiteral(VCC_HI, 0x5eed);
    for (u32 i = 0; i < 4; ++i) c.VMovLiteral(i, std::bit_cast<u32>(RenderColor[i]));
    c.Exp(0, 0xF, true, 0, 1, 2, 3, true);
    c.SEndpgm();
    t.code = std::move(c.words);
    t.hash = 0xf5c0104ull;
}

void Assemble(Template& t) {
    using namespace GcnAsm;
    if (t.render) {
        AssembleRender(t);
        return;
    }
    if (t.fragment && t.helper) {
        AssembleColorFs(t);
        return;
    }
    if (t.vertex && !t.compile_only) {
        AssembleVertex(t);
        return;
    }
    if (t.compile_only && t.wave == Wave::CtrlPhi) {
        AssembleCtrlPhi(t);
        return;
    }
    if (t.compile_only && !t.compute_compile) {
        AssembleStage(t);
        return;
    }
    Code c;
    c.SMovLiteral(VCC_HI, 0x5eed); // the recompiler expects s_mov_b32 vcc_hi, #imm first
    // v15 = local invocation index = x + y * size_x (v0, v1 = local id x, y)
    u32 log2x = 0;
    while ((1u << log2x) < t.wg[0]) ++log2x;
    c.VLshlrevB32(15, Inline(log2x), 1);
    c.Vop2(37, 15, 15, Vgpr(0)); // v_add_i32 v15, v0, v15
    c.VLshlrevB32(1, Inline(5), 15);
    Code::BufFields p{.offen = true, .vaddr = 1, .vdata = 2, .srsrc = 4};
    c.Mubuf(BUFFER_LOAD_DWORDX4, p); // v2 active, v3 vindex, v4 voffset, v5 data0
    p.offset = 16;
    p.vdata = 6;
    c.Mubuf(BUFFER_LOAD_DWORDX4, p); // v6..v8 data1..3
    c.SWaitcntAll();
    for (u32 i = 0; i < 4; ++i) c.VMovLiteral(10 + i, VgprMarker + i);
    const bool smem = t.smem != SmemSrc::None;
    if (t.wave == Wave::Lds) {
        AssembleLds(c, t.lds_bits);
    } else if (t.wave != Wave::None) {
        AssembleWave(c, t.wave);
    } else if (smem || t.combo) {
        for (u32 i = 0; i < std::max<u32>(4, smem ? t.op.ncomp : 1); ++i) c.SMovLiteral(16 + i, SgprMarker + i);
    }
    if (t.wave != Wave::None) {
    } else if (smem) {
        c.SBufferLoad(t.op.ncomp, 16, 0, t.smem == SmemSrc::Imm, t.smem == SmemSrc::Imm ? t.smem_imm : 13);
        c.SWaitcntAll();
        for (u32 i = 0; i < 4; ++i) c.VMov(10 + i, 16 + t.smem_pick[i]);
    } else {
        if (t.combo) {
            c.SBufferLoad(1, 16, 0, true, t.smem_imm);
            c.SWaitcntAll();
        }
        if (t.srt) {
            c.SLoadDwordx4(16, 2, 0); // V# from the table at s[2:3]
            c.SLoadDword(20, 2, 4);   // a constant from the same table
            c.SWaitcntAll();
        }
        c.VCmpNeU32(Inline(0), 2);
        c.SAndSaveexecB64(t.srt ? 22 : 20, VCC_LO);
        Code::BufFields f{.offset = t.inst_offset, .offen = t.offen, .idxen = t.idxen, .glc = t.glc};
        f.vaddr = t.idxen ? 3 : (t.offen ? 4 : 3);
        f.vdata = t.op.kind == GCN_LOAD ? (t.dead ? 20 : 10) : 5;
        f.srsrc = t.srt ? 16 : 0;
        f.soffset = t.soff == SoffSrc::Sgpr ? 12 : Inline(t.soff_inline);
        if (t.mtbuf) {
            c.Mtbuf(t.opcode, t.op.inst_dfmt, t.op.inst_nfmt, f);
        } else {
            c.Mubuf(t.opcode, f);
        }
        c.SWaitcntAll();
        if (t.op.kind == GCN_ATOMIC && t.glc) c.VMov(10, Vgpr(5));
        if (t.op.kind == GCN_ATOMIC && t.glc && t.op.comp_bytes == 8) c.VMov(11, Vgpr(6));
        if (t.combo) c.VMov(11, 16);
        c.SMovB64(EXEC_LO, t.srt ? 22 : 20);
    }
    if (t.srt) c.VMov(11, 20);
    c.VLshlrevB32(14, Inline(4), 15);
    Code::BufFields o{.offen = true, .vaddr = 14, .vdata = 10, .srsrc = 8};
    c.Mubuf(BUFFER_STORE_DWORDX4, o);
    c.SWaitcntAll();
    c.SEndpgm();
    t.code = std::move(c.words);
    u64 h = 0xcbf29ce484222325ull;
    for (u32 w : t.code) h = (h ^ w) * 0x100000001b3ull;
    t.hash = h;
}

struct TArgs {
    bool idxen = false, offen = true, glc = false;
    u32 inst_offset = 0;
    SoffSrc soff = SoffSrc::Inline;
    u32 soff_inline = 0;
};

u32 AddTemplate(Template t) {
    for (u32 i = 0; i < g_templates.size(); ++i) {
        if (g_templates[i].name == t.name) return i;
    }
    Assemble(t);
    g_templates.push_back(std::move(t));
    return u32(g_templates.size() - 1);
}

std::string ArgName(const TArgs& a) {
    std::string s;
    if (a.idxen) s += " idxen";
    if (a.offen) s += " offen";
    if (a.inst_offset) s += " offset:" + std::to_string(a.inst_offset);
    s += a.soff == SoffSrc::Sgpr ? " soff:s12" : " soff:" + std::to_string(a.soff_inline);
    if (a.glc) s += " glc";
    return s;
}

Template Base(std::string name, gcn_kind kind, u32 opcode, const TArgs& a) {
    Template t;
    t.name = name + ArgName(a);
    t.op.kind = kind;
    t.opcode = opcode;
    t.idxen = a.idxen;
    t.offen = a.offen;
    t.glc = a.glc;
    t.inst_offset = a.inst_offset;
    t.soff = a.soff;
    t.soff_inline = a.soff_inline;
    return t;
}

u32 RawOp(const char* name, gcn_kind kind, u32 opcode, u32 bytes, u32 ncomp, bool sign, const TArgs& a) {
    Template t = Base(name, kind, opcode, a);
    t.op.comp_bytes = bytes;
    t.op.ncomp = ncomp;
    t.op.sign_extend = sign;
    return AddTemplate(t);
}

u32 FormatOp(const char* name, gcn_kind kind, u32 opcode, u32 ncomp, const TArgs& a) {
    Template t = Base(name, kind, opcode, a);
    t.op.typed = 1;
    t.op.ncomp = ncomp;
    return AddTemplate(t);
}

u32 TbufOp(const char* name, gcn_kind kind, u32 opcode, u32 ncomp, u32 dfmt, u32 nfmt, const TArgs& a) {
    Template t = Base(std::string(name) + " dfmt:" + std::to_string(dfmt) + " nfmt:" + std::to_string(nfmt),
                      kind, opcode, a);
    t.mtbuf = true;
    t.op.typed = 1;
    t.op.inst_typed = 1;
    t.op.inst_dfmt = dfmt;
    t.op.inst_nfmt = nfmt;
    t.op.ncomp = ncomp;
    return AddTemplate(t);
}

u32 AtomicOp(const char* name, u32 opcode, gcn_atomic at, bool x2, const TArgs& a) {
    Template t = Base(name, GCN_ATOMIC, opcode, a);
    t.op.atomic = at;
    t.op.comp_bytes = x2 ? 8 : 4;
    t.op.ncomp = 1;
    t.op.atomic_return = a.glc;
    return AddTemplate(t);
}

u32 SmemOp(u32 count, SmemSrc src, u32 imm) {
    Template t;
    if (count == 8) t.smem_pick = {0, 3, 4, 7};
    if (count == 16) t.smem_pick = {0, 7, 8, 15};
    t.name = "s_buffer_load_dword" + (count > 1 ? "x" + std::to_string(count) : std::string{}) +
             (src == SmemSrc::Imm ? " imm:" + std::to_string(imm) : std::string(" soff:s13"));
    t.op.kind = GCN_SLOAD;
    t.op.ncomp = count;
    t.smem = src;
    t.smem_imm = imm;
    return AddTemplate(t);
}

u32 ComboOp(u32 imm) {
    Template t = Base("s_buffer_load_dword imm:" + std::to_string(imm) + " + buffer_load_dword", GCN_LOAD,
                      GcnAsm::BUFFER_LOAD_DWORD, TArgs{});
    t.op.comp_bytes = 4;
    t.op.ncomp = 1;
    t.combo = true;
    t.smem_imm = imm;
    return AddTemplate(t);
}

// ---------------------------------------------------------------------------------------------
// V# helpers

gcn_vsharp RawV(u32 num_records, u32 dfmt = GCN_DFMT_32, u32 nfmt = GCN_NFMT_UINT) {
    gcn_vsharp v{};
    v.num_records = num_records;
    v.dfmt = dfmt;
    v.nfmt = nfmt;
    v.dst_sel[0] = 4; v.dst_sel[1] = 5; v.dst_sel[2] = 6; v.dst_sel[3] = 7;
    v.element_size = 2;
    v.index_stride = 8;
    return v;
}
gcn_vsharp StructV(u32 stride, u32 num_records, u32 dfmt = GCN_DFMT_32, u32 nfmt = GCN_NFMT_UINT) {
    gcn_vsharp v = RawV(num_records, dfmt, nfmt);
    v.stride = stride;
    return v;
}
gcn_vsharp SwizzleV(u32 stride, u32 num_records, u32 element_size, u32 index_stride) {
    gcn_vsharp v = StructV(stride, num_records);
    v.swizzle_en = 1;
    v.element_size = element_size;
    v.index_stride = index_stride;
    return v;
}

std::string Hex(u64 x) {
    char b[32];
    std::snprintf(b, sizeof(b), "%#" PRIx64, x);
    return b;
}

std::string VName(const gcn_vsharp& v) {
    std::string s = v.stride == 0 ? "raw" : (v.swizzle_en ? "swizzle" : "struct");
    s += " stride=" + std::to_string(v.stride) + " nr=" + Hex(v.num_records);
    if (v.swizzle_en) s += " es=" + std::to_string(v.element_size) + " is=" + std::to_string(v.index_stride);
    if (v.add_tid) s += " add_tid";
    s += " dfmt=" + std::to_string(v.dfmt) + " nfmt=" + std::to_string(v.nfmt);
    return s;
}

Job& NewJob(const std::string& group, u32 tmpl, const gcn_vsharp& v, u64 base_delta, u32 sgpr_soff = 0) {
    Job j;
    j.group = group;
    j.tmpl = tmpl;
    j.v = v;
    j.base_delta = base_delta;
    j.sgpr_soff = sgpr_soff;
    const Template& t = g_templates[tmpl];
    const bool dyn_or_nonzero = t.smem == SmemSrc::None && !t.combo &&
                                (t.soff == SoffSrc::Sgpr || t.soff_inline != 0);
    j.approved_struct_soff = v.stride != 0 && dyn_or_nonzero;
    j.approved_swizzle = v.swizzle_en && v.index_stride && v.num_records % v.index_stride != 0;
    j.approved_offen_only = v.stride != 0 && t.offen && !t.idxen && !v.add_tid && t.wave == Wave::None &&
                            t.smem == SmemSrc::None;
    // §8.1.5: const_element_size "Must be >= maximum element size" and const_stride "must be an
    // integer multiple of const_element_size".
    if (v.swizzle_en && v.element_size) {
        const u32 access = t.op.typed ? 4 : (t.op.comp_bytes ? t.op.comp_bytes : 4);
        j.isa_invalid = v.element_size < access || v.stride % v.element_size != 0;
    }
    j.label = t.name + " | " + VName(v) + " | base+" + std::to_string(base_delta);
    if (t.soff == SoffSrc::Sgpr && t.smem == SmemSrc::None) j.label += " s12=" + Hex(sgpr_soff);
    g_jobs.push_back(std::move(j));
    return g_jobs.back();
}

u32 Soff(const Template& t, const Job& j) {
    return t.soff == SoffSrc::Sgpr ? j.sgpr_soff : t.soff_inline;
}

void SetLane(Job& j, u32 lane, u32 vindex, u32 voffset, std::array<u32, 4> data = {}) {
    j.lanes[lane].active = 1;
    j.lanes[lane].vindex = vindex;
    j.lanes[lane].voffset = voffset;
    for (u32 i = 0; i < 4; ++i) j.lanes[lane].data[i] = data[i];
}

std::vector<u32> Uniq(std::vector<u64> in, size_t cap = 64) {
    std::vector<u32> out;
    std::set<u32> seen;
    for (u64 x : in) {
        if (x > 0xFFFFFFFFull) continue;
        if (seen.insert(u32(x)).second) out.push_back(u32(x));
        if (out.size() == cap) break;
    }
    return out;
}

// Effective raw buffer offsets around the end of the range (lim = num_records - soffset).
std::vector<u64> RawProbe(u64 lim) {
    std::vector<u64> b{0, 1, 2, 3, 4, 5, 6, 8, 12};
    for (int d = -12; d <= 6; ++d) {
        if (int64_t(lim) + d >= 0) b.push_back(lim + d);
    }
    for (u64 x : {lim + 8, lim + 16, lim + 64, u64(0x100), u64(0x1000), u64(0x10000),
                  u64(0x7FFFFFFC), u64(0xFFFFFFF0), u64(0xFFFFFFFC), u64(0xFFFFFFFE), u64(0xFFFFFFFF)}) {
        b.push_back(x);
    }
    return b;
}

std::array<u32, 4> StoreData(u32 salt) {
    return {0xA1B2C3D4u ^ salt, 0x95867768u ^ salt, 0x1A2B3C4Du ^ salt, 0x77665544u ^ salt};
}

// ---------------------------------------------------------------------------------------------
// The case matrix (README.md lists the coverage).

void AddLoadCases() {
    using namespace GcnAsm;
    const TArgs offen{};
    TArgs offen_s{.inst_offset = 4, .soff = SoffSrc::Sgpr};
    TArgs offen_big{.inst_offset = 4092, .soff = SoffSrc::Sgpr};
    TArgs offen_s64{.soff_inline = 64};
    struct L { const char* name; u32 op, bytes, n; bool sign; };
    const L raw_loads[] = {{"buffer_load_dword", BUFFER_LOAD_DWORD, 4, 1, false},
                           {"buffer_load_dwordx2", BUFFER_LOAD_DWORDX2, 4, 2, false},
                           {"buffer_load_dwordx3", BUFFER_LOAD_DWORDX3, 4, 3, false},
                           {"buffer_load_dwordx4", BUFFER_LOAD_DWORDX4, 4, 4, false},
                           {"buffer_load_ubyte", BUFFER_LOAD_UBYTE, 1, 1, false},
                           {"buffer_load_sbyte", BUFFER_LOAD_SBYTE, 1, 1, true},
                           {"buffer_load_ushort", BUFFER_LOAD_USHORT, 2, 1, false},
                           {"buffer_load_sshort", BUFFER_LOAD_SSHORT, 2, 1, true}};

    // Raw (stride 0): exact last valid byte/dword, first invalid, per-component xN, misaligned
    // base (binding adjust), 32-bit wrap of voffset+inst_offset.
    for (const auto& l : raw_loads) {
        const u32 t = RawOp(l.name, GCN_LOAD, l.op, l.bytes, l.n, l.sign, offen);
        for (u32 nr : {0u, 6u, 7u, 8u, 13u, 64u}) {
            for (u64 mis : {0ull, 4ull, 12ull}) {
                if (l.bytes == 4 && l.n > 1 && mis == 12 && nr != 64) continue;
                Job& j = NewJob("raw-load", t, RawV(nr), mis);
                const auto offs = Uniq(RawProbe(nr));
                for (u32 i = 0; i < offs.size(); ++i) SetLane(j, i, 0xFFFFFFFFu - i, offs[i]);
            }
        }
        if (l.bytes < 4) {
            Job& j = NewJob("raw-load", t, RawV(7), 2); // base not 4-aligned
            const auto offs = Uniq(RawProbe(7));
            for (u32 i = 0; i < offs.size(); ++i) SetLane(j, i, 0, offs[i]);
        }
    }
    // Raw with SGPR / inline soffset and inst_offset: the range shrinks by soffset.
    for (const auto& l : {raw_loads[0], raw_loads[3]}) {
        for (const TArgs& a : {offen_s, offen_big}) {
            const u32 t = RawOp(l.name, GCN_LOAD, l.op, l.bytes, l.n, false, a);
            for (u32 nr : {16u, 64u}) {
                for (u32 soff : {0u, 4u, 6u, 8u, nr, nr + 16}) {
                    Job& j = NewJob("raw-soffset", t, RawV(nr), 0, soff);
                    std::vector<u64> b = RawProbe(nr >= soff ? nr - soff : 0);
                    std::vector<u64> vo;
                    for (u64 x : b) vo.push_back(u32(x - a.inst_offset));
                    vo.push_back(u32(0xFFFFFFFFu - a.inst_offset + 1 + 4)); // voffset+inst_offset wraps to 4
                    const auto offs = Uniq(vo);
                    for (u32 i = 0; i < offs.size(); ++i) SetLane(j, i, 0, offs[i]);
                }
            }
        }
        const u32 t = RawOp(l.name, GCN_LOAD, l.op, l.bytes, l.n, false, offen_s64);
        Job& j = NewJob("raw-soffset", t, RawV(100), 0);
        const auto offs = Uniq(RawProbe(36));
        for (u32 i = 0; i < offs.size(); ++i) SetLane(j, i, 0, offs[i]);
    }
    // Raw, neither idxen nor offen: constant address (inst_offset + soffset).
    {
        const u32 t = RawOp("buffer_load_dwordx4", GCN_LOAD, BUFFER_LOAD_DWORDX4, 4, 4, false,
                            TArgs{.offen = false, .inst_offset = 8, .soff = SoffSrc::Sgpr});
        for (u32 soff : {0u, 4u, 8u, 12u, 16u}) {
            Job& j = NewJob("raw-const", t, RawV(28), 0, soff);
            SetLane(j, 0, 0, 0);
            SetLane(j, 1, 0, 0);
        }
    }
    // Raw buffer with idxen: stride 0 means index * 0; the index must not matter.
    {
        const u32 t = RawOp("buffer_load_dwordx2", GCN_LOAD, BUFFER_LOAD_DWORDX2, 4, 2, false,
                            TArgs{.idxen = true, .offen = true});
        Job& j = NewJob("raw-idxen", t, RawV(24), 0);
        const u32 idx[] = {0u, 1u, 5u, 24u, 0x10000001u, 0xFFFFFFFFu};
        const u32 off[] = {0u, 8u, 16u, 20u, 22u, 24u, 0xFFFFFFFCu};
        u32 lane = 0;
        for (u32 i : idx) for (u32 o : off) if (lane < 64) SetLane(j, lane++, i, o);
    }

    // Structured: index vs num_records (never index*stride), offset vs stride with idxen,
    // 32-bit wrap of index*stride, num_records 0, per-component xN at the stride end.
    const TArgs io{.idxen = true, .offen = true};
    const TArgs i_only{.idxen = true, .offen = false, .inst_offset = 4};
    struct SV { u32 stride, nr; };
    const SV structs[] = {{4, 16}, {12, 5}, {16, 1}, {16, 16}, {20, 7}, {48, 3}, {16, 0}};
    for (const auto& l : {raw_loads[0], raw_loads[1], raw_loads[3], raw_loads[4], raw_loads[6]}) {
        const u32 t = RawOp(l.name, GCN_LOAD, l.op, l.bytes, l.n, l.sign, io);
        for (const auto& s : structs) {
            for (u64 mis : {0ull, 4ull}) {
                Job& j = NewJob("struct-load", t, StructV(s.stride, s.nr), mis);
                const auto idx = Uniq({0, 1, s.nr ? s.nr - 1ull : 0, s.nr, s.nr + 1ull, 0x10000001, 0x80000000,
                                       0xFFFFFFFF});
                const auto off = Uniq({0, 4, s.stride >= 4 ? s.stride - 4ull : 0, s.stride - 2ull,
                                       s.stride - 1ull, s.stride, s.stride + 4ull, 0xFFFFFFFC});
                u32 lane = 0;
                for (u32 i : idx) for (u32 o : off) if (lane < 64) SetLane(j, lane++, i, o);
            }
        }
    }
    {
        const u32 t = RawOp("buffer_load_dword", GCN_LOAD, BUFFER_LOAD_DWORD, 4, 1, false, i_only);
        for (const auto& s : structs) {
            Job& j = NewJob("struct-load", t, StructV(s.stride, s.nr), 0);
            const auto idx = Uniq({0, 1, 2, s.nr ? s.nr - 1ull : 0, s.nr, s.nr + 1ull, 0x10000000, 0x10000001,
                                   0x80000000, 0xFFFFFFFF});
            for (u32 i = 0; i < idx.size(); ++i) SetLane(j, i, idx[i], 0);
        }
    }
    // Structured with offen only: GCN checks index 0 < num_records but never the offset, so
    // offsets past stride*num_records are legal reads beyond the bound range.
    {
        const u32 t = RawOp("buffer_load_dword", GCN_LOAD, BUFFER_LOAD_DWORD, 4, 1, false, offen);
        for (const auto& s : {SV{16, 4}, SV{12, 5}, SV{16, 0}}) {
            Job& j = NewJob("struct-offen-only", t, StructV(s.stride, s.nr), 0);
            const u64 size = u64(s.stride) * s.nr;
            const auto off = Uniq({0, 4, s.stride, size >= 4 ? size - 4 : 0, size, size + 4, 0x1000, 0xFFFFFFFC});
            for (u32 i = 0; i < off.size(); ++i) SetLane(j, i, 0, off[i]);
        }
    }
    // Structured with soffset (orchestrator-approved loud: nonzero or dynamic SGPR soffset).
    {
        const u32 t_inl = RawOp("buffer_load_dword", GCN_LOAD, BUFFER_LOAD_DWORD, 4, 1, false,
                                TArgs{.idxen = true, .offen = true, .soff_inline = 16});
        const u32 t_sgpr = RawOp("buffer_load_dword", GCN_LOAD, BUFFER_LOAD_DWORD, 4, 1, false,
                                 TArgs{.idxen = true, .offen = true, .soff = SoffSrc::Sgpr});
        for (auto [t, soff] : {std::pair{t_inl, 0u}, std::pair{t_sgpr, 0u}, std::pair{t_sgpr, 16u},
                               std::pair{t_sgpr, 64u}}) {
            Job& j = NewJob("struct-soffset", t, StructV(16, 4), 0, soff);
            u32 lane = 0;
            for (u32 i : {0u, 1u, 3u, 4u}) for (u32 o : {0u, 12u, 16u}) SetLane(j, lane++, i, o);
        }
    }

    // Swizzled (§8.1.5.2): byte and dword probes over index/offset including element and
    // index_stride boundaries; incomplete last group is an approved loud binder check.
    struct SW { u32 stride, nr, es, is; };
    for (const auto& s : {SW{16, 16, 4, 8}, SW{32, 32, 16, 16}, SW{8, 16, 2, 8}, SW{16, 12, 4, 8},
                          SW{16, 8, 8, 8}}) {
        for (const auto& l : {raw_loads[0], raw_loads[4]}) {
            const u32 t = RawOp(l.name, GCN_LOAD, l.op, l.bytes, l.n, false, io);
            Job& j = NewJob("swizzle-load", t, SwizzleV(s.stride, s.nr, s.es, s.is), 0);
            const auto idx = Uniq({0, 1, s.is - 1ull, s.is, s.nr - 1ull, s.nr, s.nr + 1ull, 0xFFFFFFFF});
            const auto off = Uniq({0, 1, s.es - 1ull, s.es, s.stride - 4ull, s.stride - 1ull, s.stride, 0xFFFFFFFC});
            u32 lane = 0;
            for (u32 i : idx) for (u32 o : off) if (lane < 64) SetLane(j, lane++, i, o);
        }
    }

    // add_tid: index += lane id (thread_id[5:0]); offset checked against stride even without
    // idxen. All 64 lanes active.
    for (const TArgs& a : {io, offen}) {
        const u32 t = RawOp("buffer_load_dword", GCN_LOAD, BUFFER_LOAD_DWORD, 4, 1, false, a);
        for (const auto& s : {SV{16, 40}, SV{4, 70}, SV{8, 64}}) {
            gcn_vsharp v = StructV(s.stride, s.nr);
            v.add_tid = 1;
            Job& j = NewJob("add-tid", t, v, 0);
            for (u32 lane = 0; lane < 64; ++lane) {
                SetLane(j, lane, lane % 3 == 0 ? 0 : 1, (lane % 5) * 4);
            }
        }
    }

    // Formatted (all-or-nothing, dst_sel from the resource).
    const TArgs fio{.idxen = true, .offen = true};
    {
        const u32 xyzw = FormatOp("buffer_load_format_xyzw", GCN_LOAD, BUFFER_LOAD_FORMAT_XYZW, 4, fio);
        const u32 xy = FormatOp("buffer_load_format_xy", GCN_LOAD, BUFFER_LOAD_FORMAT_XY, 2, fio);
        gcn_vsharp a = StructV(16, 4, GCN_DFMT_32_32_32_32, GCN_NFMT_UINT);
        gcn_vsharp b = StructV(8, 5, GCN_DFMT_32_32, GCN_NFMT_FLOAT);
        b.dst_sel[2] = 0; b.dst_sel[3] = 1; // X Y 0 1
        gcn_vsharp c = StructV(4, 6, GCN_DFMT_8_8_8_8, GCN_NFMT_UINT);
        gcn_vsharp d = StructV(12, 3, GCN_DFMT_32_32_32, GCN_NFMT_UINT);
        d.dst_sel[3] = 1;
        gcn_vsharp e = StructV(4, 8, GCN_DFMT_16_16, GCN_NFMT_UINT);
        e.dst_sel[0] = 5; e.dst_sel[1] = 4; e.dst_sel[2] = 1; e.dst_sel[3] = 0; // Y X 1 0
        for (const gcn_vsharp& v : {a, b, c, d, e}) {
            for (u32 t : {xyzw, xy}) {
                Job& j = NewJob("format-load", t, v, 0);
                const auto idx = Uniq({0, 1, v.num_records - 1ull, v.num_records, v.num_records + 1ull, 0x10000001});
                const auto off = Uniq({0, 4, v.stride - 4ull, v.stride, 0xFFFFFFFC});
                u32 lane = 0;
                for (u32 i : idx) for (u32 o : off) if (lane < 64) SetLane(j, lane++, i, o);
            }
        }
        const u32 x_raw = FormatOp("buffer_load_format_x", GCN_LOAD, BUFFER_LOAD_FORMAT_X, 1, offen);
        const u32 xyzw_raw = FormatOp("buffer_load_format_xyzw", GCN_LOAD, BUFFER_LOAD_FORMAT_XYZW, 4, offen);
        for (u32 t : {x_raw, xyzw_raw}) {
            for (u32 nr : {10u, 16u, 32u}) {
                Job& j = NewJob("format-load", t, RawV(nr, GCN_DFMT_32_32_32_32, GCN_NFMT_UINT), 0);
                const auto off = Uniq({0, 4, 8, 12, 16, 20, 24, 28, 32, 0xFFFFFFF0});
                for (u32 i = 0; i < off.size(); ++i) SetLane(j, i, 0, off[i]);
            }
        }
        // TBUFFER: format from the instruction (identity dst_sel).
        const u32 tb = TbufOp("tbuffer_load_format_xy", GCN_LOAD, TBUFFER_LOAD_FORMAT_XY, 2, GCN_DFMT_32_32,
                              GCN_NFMT_UINT, fio);
        const u32 tb4 = TbufOp("tbuffer_load_format_xyzw", GCN_LOAD, TBUFFER_LOAD_FORMAT_XYZW, 4,
                               GCN_DFMT_8_8_8_8, GCN_NFMT_UINT, fio);
        for (u32 t : {tb, tb4}) {
            for (const gcn_vsharp& v : {StructV(8, 5), StructV(16, 3), StructV(8, 5, GCN_DFMT_INVALID)}) {
                Job& j = NewJob("tbuffer-load", t, v, 0);
                const auto idx = Uniq({0, v.num_records - 1ull, v.num_records, 0x10000001});
                const auto off = Uniq({0, 4, v.stride - 4ull, v.stride - 2ull, v.stride});
                u32 lane = 0;
                for (u32 i : idx) for (u32 o : off) if (lane < 64) SetLane(j, lane++, i, o);
            }
        }
    }

    // Resource data format INVALID: unbound for MUBUF (§8.1.4 NOTE).
    for (const auto& l : {raw_loads[0], raw_loads[3], raw_loads[4]}) {
        const u32 t = RawOp(l.name, GCN_LOAD, l.op, l.bytes, l.n, false, offen);
        Job& j = NewJob("invalid-dfmt", t, RawV(64, GCN_DFMT_INVALID, 0), 0);
        const auto off = Uniq({0, 4, 32, 60, 63, 64});
        for (u32 i = 0; i < off.size(); ++i) SetLane(j, i, 0, off[i]);
    }
    {
        const u32 t = FormatOp("buffer_load_format_xyzw", GCN_LOAD, GcnAsm::BUFFER_LOAD_FORMAT_XYZW, 4, offen);
        Job& j = NewJob("invalid-dfmt", t, RawV(64, GCN_DFMT_INVALID, 0), 0);
        SetLane(j, 0, 0, 0);
        SetLane(j, 1, 0, 16);
    }

    // Descriptor extremes: all-zero V#; GetSize() u32 wrap (stride*num_records = 2^32 -> 0, or
    // small); num_records 0xFFFFFFFF.
    {
        const u32 t = RawOp("buffer_load_dword", GCN_LOAD, BUFFER_LOAD_DWORD, 4, 1, false, io);
        Job& z = NewJob("extreme", t, gcn_vsharp{}, 0);
        z.zero_sharp = true;
        z.label = g_templates[t].name + " | all-zero V#";
        for (u32 i = 0; i < 8; ++i) SetLane(z, i, i, i * 4);
        for (const gcn_vsharp& v : {StructV(16, 0x10000000u), StructV(4, 0x40000001u), StructV(16, 0xFFFFFFFFu),
                                    RawV(0xFFFFFFFFu)}) {
            Job& j = NewJob("extreme", t, v, 0);
            const auto idx = Uniq({0, 1, 2, 0x0FFFFFFF, 0x10000000, 0x40000000});
            for (u32 i = 0; i < idx.size(); ++i) SetLane(j, i, idx[i], 0);
        }
    }

    // Scalar buffer loads (§7.2.2): per-dword clamp, ">" for stride != 0.
    {
        for (u32 imm : {0u, 1u, 14u, 15u, 16u, 17u, 255u}) {
            const u32 t = SmemOp(1, SmemSrc::Imm, imm);
            for (const gcn_vsharp& v : {RawV(64), RawV(62), StructV(16, 4), RawV(0), RawV(64, GCN_DFMT_INVALID, 0)}) {
                Job& j = NewJob("smem", t, v, 0);
                for (u32 lane = 0; lane < 64; ++lane) SetLane(j, lane, 0, 0);
            }
        }
        const u32 t4 = SmemOp(4, SmemSrc::Sgpr, 0);
        for (const gcn_vsharp& v : {RawV(64), RawV(62), StructV(16, 4)}) {
            for (u32 off : {0u, 6u, 48u, 52u, 56u, 60u, 64u, 68u, 0xFFFFFFFCu}) {
                Job& j = NewJob("smem", t4, v, 0);
                j.smem_sgpr = off;
                j.label += " s13=" + Hex(off);
                for (u32 lane = 0; lane < 64; ++lane) SetLane(j, lane, 0, 0);
            }
        }
        // Constant-buffer sized loads (x8/x16): last dword in range, first dword out, straddles.
        for (u32 count : {8u, 16u}) {
            for (u32 imm : {0u, 4u, 8u, 12u, 16u, 20u, 24u, 60u}) {
                const u32 t = SmemOp(count, SmemSrc::Imm, imm);
                for (const gcn_vsharp& v : {RawV(64), RawV(100), RawV(0x80), StructV(16, 4), StructV(16, 8)}) {
                    Job& j = NewJob("smem", t, v, 0);
                    for (u32 lane = 0; lane < 64; ++lane) SetLane(j, lane, 0, 0);
                }
            }
            const u32 ts = SmemOp(count, SmemSrc::Sgpr, 0);
            for (u32 off : {0u, 32u, 64u, 66u, 96u, 0x100u, 0xFFFFFFC0u}) {
                Job& j = NewJob("smem", ts, RawV(128), 0);
                j.smem_sgpr = off;
                j.label += " s13=" + Hex(off);
                for (u32 lane = 0; lane < 64; ++lane) SetLane(j, lane, 0, 0);
            }
        }
        // One V# used by both s_buffer_load and buffer_load: INVALID dfmt zeroes only the
        // vector load (§7.2.2 ignores the format).
        const u32 combo = ComboOp(2);
        for (const gcn_vsharp& v : {RawV(64), RawV(64, GCN_DFMT_INVALID, 0)}) {
            Job& j = NewJob("smem", combo, v, 0);
            for (u32 lane = 0; lane < 8; ++lane) SetLane(j, lane, 0, lane * 4);
        }
    }
}

void AddStoreCases() {
    using namespace GcnAsm;
    u32 salt = 1;
    struct S { const char* name; u32 op, bytes, n; };
    const S raw_stores[] = {{"buffer_store_dword", BUFFER_STORE_DWORD, 4, 1},
                            {"buffer_store_dwordx2", BUFFER_STORE_DWORDX2, 4, 2},
                            {"buffer_store_dwordx3", BUFFER_STORE_DWORDX3, 4, 3},
                            {"buffer_store_dwordx4", BUFFER_STORE_DWORDX4, 4, 4},
                            {"buffer_store_byte", BUFFER_STORE_BYTE, 1, 1},
                            {"buffer_store_short", BUFFER_STORE_SHORT, 2, 1}};
    for (const auto& s : raw_stores) {
        const u32 t = RawOp(s.name, GCN_STORE, s.op, s.bytes, s.n, false, TArgs{});
        for (u32 nr : {7u, 8u, 64u}) {
            for (u64 mis : {0ull, 4ull}) {
                for (u64 b : {nr - 16ull, nr - 8ull, nr - 6ull, nr - 4ull, nr - 3ull, nr - 2ull, nr - 1ull,
                              u64(nr), u64(0xFFFFFFFC)}) {
                    if (int64_t(b) < 0 && b < 0xFFFF0000ull) continue;
                    Job& j = NewJob("raw-store", t, RawV(nr), mis);
                    SetLane(j, 0, 0, u32(b), StoreData(salt++));
                    j.label += " voff=" + Hex(u32(b));
                }
            }
        }
    }
    {
        const u32 t = RawOp("buffer_store_dwordx2", GCN_STORE, BUFFER_STORE_DWORDX2, 4, 2, false,
                            TArgs{.inst_offset = 4, .soff = SoffSrc::Sgpr});
        for (u32 soff : {0u, 8u, 40u}) {
            for (u32 b : {20u, 24u, 26u, 28u, 0xFFFFFFFCu}) {
                Job& j = NewJob("raw-store", t, RawV(32), 0, soff);
                SetLane(j, 0, 0, b - 4, StoreData(salt++));
                j.label += " voff=" + Hex(b - 4);
            }
        }
    }
    const TArgs io{.idxen = true, .offen = true};
    {
        const u32 x4 = RawOp("buffer_store_dwordx4", GCN_STORE, BUFFER_STORE_DWORDX4, 4, 4, false, io);
        const u32 fmt = FormatOp("buffer_store_format_xyzw", GCN_STORE, BUFFER_STORE_FORMAT_XYZW, 4, io);
        const u32 tb = TbufOp("tbuffer_store_format_xy", GCN_STORE, TBUFFER_STORE_FORMAT_XY, 2, GCN_DFMT_32_32,
                              GCN_NFMT_UINT, io);
        const u32 by = RawOp("buffer_store_byte", GCN_STORE, BUFFER_STORE_BYTE, 1, 1, false, io);
        struct P { u32 i, o; };
        for (u32 t : {x4, fmt, tb, by}) {
            for (const gcn_vsharp& v0 : {StructV(16, 4), StructV(20, 3), StructV(8, 5)}) {
                gcn_vsharp v = v0;
                if (t == fmt) {
                    v.dfmt = GCN_DFMT_32_32_32_32;
                    if (v.stride < 16) continue;
                }
                const u32 nr = v.num_records, st = v.stride;
                for (P p : {P{nr - 1, 0}, P{nr - 1, st - 4}, P{nr - 1, st - 8}, P{nr - 1, st - 1}, P{nr, 0},
                            P{0x10000001, 0}, P{0, st}, P{1, 4}}) {
                    Job& j = NewJob("struct-store", t, v, 0);
                    SetLane(j, 0, p.i, p.o, StoreData(salt++));
                    j.label += " idx=" + Hex(p.i) + " off=" + Hex(p.o);
                }
            }
        }
        // swizzled byte/dword stores
        const u32 dw = RawOp("buffer_store_dword", GCN_STORE, BUFFER_STORE_DWORD, 4, 1, false, io);
        for (u32 t : {by, dw}) {
            for (P p : {P{0, 0}, P{7, 3}, P{8, 2}, P{15, 15}, P{16, 0}, P{3, 16}}) {
                Job& j = NewJob("swizzle-store", t, SwizzleV(16, 16, 4, 8), 0);
                SetLane(j, 0, p.i, p.o, StoreData(salt++));
                j.label += " idx=" + Hex(p.i) + " off=" + Hex(p.o);
            }
        }
        // structured store with soffset (approved loud) and offen-only beyond the bound range
        const u32 so = RawOp("buffer_store_dword", GCN_STORE, BUFFER_STORE_DWORD, 4, 1, false,
                             TArgs{.idxen = true, .offen = true, .soff_inline = 16});
        Job& j1 = NewJob("struct-soffset", so, StructV(16, 4), 0);
        SetLane(j1, 0, 3, 12, StoreData(salt++));
        const u32 oo = RawOp("buffer_store_dword", GCN_STORE, BUFFER_STORE_DWORD, 4, 1, false, TArgs{});
        for (u32 off : {60u, 64u, 128u}) {
            Job& j = NewJob("struct-offen-only", oo, StructV(16, 4), 0);
            SetLane(j, 0, 0, off, StoreData(salt++));
            j.label += " voff=" + Hex(off);
        }
    }
    // INVALID resource format: MUBUF stores dropped; TBUFFER ambiguous.
    {
        const u32 dw = RawOp("buffer_store_dword", GCN_STORE, BUFFER_STORE_DWORD, 4, 1, false, TArgs{});
        const u32 tb = TbufOp("tbuffer_store_format_xy", GCN_STORE, TBUFFER_STORE_FORMAT_XY, 2, GCN_DFMT_32_32,
                              GCN_NFMT_UINT, TArgs{});
        for (u32 t : {dw, tb}) {
            for (u32 b : {0u, 32u}) {
                Job& j = NewJob("invalid-dfmt", t, RawV(64, GCN_DFMT_INVALID, 0), 0);
                SetLane(j, 0, 0, b, StoreData(salt++));
                j.label += " voff=" + Hex(b);
            }
        }
    }
}

u32 PatternDword(u64 va) { // initial memory at a guest address inside an arena's bound region
    u32 v = 0;
    for (u32 i = 0; i < 4; ++i) v |= u32(BufferByte(va + i)) << (8 * i);
    return v;
}

void AddAtomicCases() {
    using namespace GcnAsm;
    const TArgs ret{.glc = true};
    const TArgs io_ret{.idxen = true, .offen = true, .glc = true};
    struct A { const char* name; u32 op; gcn_atomic at; bool x2; TArgs args; u32 d0, d1; };
    const A atomics[] = {
        {"buffer_atomic_add", BUFFER_ATOMIC_ADD, GCN_AT_ADD, false, ret, 0x01000001u, 0},
        {"buffer_atomic_cmpswap", BUFFER_ATOMIC_CMPSWAP, GCN_AT_CMPSWAP, false, ret, 0x12345678u, 0},
        {"buffer_atomic_umax", BUFFER_ATOMIC_UMAX, GCN_AT_UMAX, false, io_ret, 0xFFFFFFF0u, 0},
        {"buffer_atomic_inc", BUFFER_ATOMIC_INC, GCN_AT_INC, false, ret, 0xFFFFFFFFu, 0},
        {"buffer_atomic_dec", BUFFER_ATOMIC_DEC, GCN_AT_DEC, false, ret, 0xFFFFFFFFu, 0},
        {"buffer_atomic_swap", BUFFER_ATOMIC_SWAP, GCN_AT_SWAP, false,
         TArgs{.glc = true, .soff = SoffSrc::Sgpr}, 0xCAFEF00Du, 0},
        {"buffer_atomic_sub", BUFFER_ATOMIC_SUB, GCN_AT_SUB, false, TArgs{}, 0x00010001u, 0},
        {"buffer_atomic_add_x2", BUFFER_ATOMIC_ADD_X2, GCN_AT_ADD, true, TArgs{}, 0x00000001u, 0x00000001u},
    };
    for (const auto& a : atomics) {
        const u32 t = AtomicOp(a.name, a.op, a.at, a.x2, a.args);
        std::vector<std::pair<gcn_vsharp, std::vector<std::pair<u32, u32>>>> configs;
        if (a.args.idxen) {
            configs.push_back({StructV(16, 4), {{3, 12}, {3, 14}, {4, 0}, {0, 16}, {0x10000001, 0}, {1, 4}}});
        } else {
            for (u32 nr : {8u, 64u}) {
                configs.push_back({RawV(nr), {{0, nr - 8}, {0, nr - 4}, {0, nr - 3}, {0, nr - 1}, {0, nr},
                                               {0, 0xFFFFFFFC}}});
            }
        }
        for (const auto& [v, pts] : configs) {
            for (auto [idx, off] : pts) {
                const u32 soff = a.args.soff == SoffSrc::Sgpr ? 4 : 0;
                Job& j = NewJob("atomic", t, v, 0, soff);
                std::array<u32, 4> data{a.d0, a.d1, 0, 0};
                j.label += " idx=" + Hex(idx) + " off=" + Hex(off);
                if (a.at == GCN_AT_CMPSWAP) {
                    // compare value = current memory, so the swap happens when in range
                    const u64 base = GuestVa + u64(g_jobs.size() - 1) % MaxBatch * Arena + BaseInArena;
                    data[1] = PatternDword((base + soff + u64(idx) * v.stride + off) & ~3ull);
                }
                SetLane(j, 0, idx, off, data);
            }
        }
    }
    // INC/DEC DATA limits x old values (below / equal / above the limit, 0), with and without glc,
    // and out of range (unchanged memory, 0 returned).
    for (auto [name, op, at] : {std::tuple{"buffer_atomic_inc", BUFFER_ATOMIC_INC, GCN_AT_INC},
                                std::tuple{"buffer_atomic_dec", BUFFER_ATOMIC_DEC, GCN_AT_DEC}}) {
        for (bool glc : {true, false}) {
            const u32 t = AtomicOp(name, op, at, false, TArgs{.glc = glc});
            for (u32 limit : {0u, 1u, 5u, 0xFFFFFFFFu}) {
                std::vector<u64> olds{0, limit, u64(limit) + 1};
                if (limit) olds.push_back(limit - 1);
                for (u64 old : olds) {
                    if (old > 0xFFFFFFFFull) continue;
                    Job& j = NewJob("atomic-semantics", t, RawV(64), 0);
                    j.init_words = {u32(old)};
                    SetLane(j, 0, 0, 0, {limit, 0, 0, 0});
                    j.label += " DATA=" + Hex(limit) + " old=" + Hex(old);
                }
                Job& oob = NewJob("atomic-semantics", t, RawV(64), 0);
                oob.init_words = {7};
                SetLane(oob, 0, 0, 64, {limit, 0, 0, 0});
                oob.label += " DATA=" + Hex(limit) + " out of range";
            }
        }
    }
    // Every integer X2 atomic, with and without glc: run (exact or a loud capability error on
    // devices without 64-bit buffer atomics) and compile-only with the capability forced on, so
    // the native emitter is validated by spirv-val even where it cannot run.
    struct X2 { const char* name; u32 op; gcn_atomic at; };
    const X2 x2s[] = {{"buffer_atomic_swap_x2", 80, GCN_AT_SWAP}, {"buffer_atomic_cmpswap_x2", 81, GCN_AT_CMPSWAP},
                      {"buffer_atomic_add_x2", 82, GCN_AT_ADD}, {"buffer_atomic_sub_x2", 83, GCN_AT_SUB},
                      {"buffer_atomic_smin_x2", 85, GCN_AT_SMIN}, {"buffer_atomic_umin_x2", 86, GCN_AT_UMIN},
                      {"buffer_atomic_smax_x2", 87, GCN_AT_SMAX}, {"buffer_atomic_umax_x2", 88, GCN_AT_UMAX},
                      {"buffer_atomic_and_x2", 89, GCN_AT_AND}, {"buffer_atomic_or_x2", 90, GCN_AT_OR},
                      {"buffer_atomic_xor_x2", 91, GCN_AT_XOR}, {"buffer_atomic_inc_x2", 92, GCN_AT_INC},
                      {"buffer_atomic_dec_x2", 93, GCN_AT_DEC}};
    for (const X2& x : x2s) {
        for (bool glc : {false, true}) {
            const u32 t = AtomicOp(x.name, x.op, x.at, true, TArgs{.glc = glc});
            for (u32 off : {8u, 60u, 64u}) {
                Job& j = NewJob("atomic-x2", t, RawV(64), 0);
                for (u32 k = 0; k < 16; ++k) j.init_words.push_back(0x10000000u * (k & 3) + 0x111u * k + 3);
                const u32 lo = j.init_words[off / 4 % 16], hi = j.init_words[(off / 4 + 1) % 16];
                SetLane(j, 0, 0, off, {0x00000005u, 0x00000001u, x.at == GCN_AT_CMPSWAP ? lo : 0u, x.at == GCN_AT_CMPSWAP ? hi : 0u});
                j.label += " off=" + Hex(off);
            }
            Template c = g_templates[t];
            c.name += " (compile, int64 atomics forced)";
            c.compile_only = true;
            c.compute_compile = true;
            c.force_int64_atomics = true;
            c.code.clear();
            const u32 ct = AddTemplate(c);
            NewJob("atomic-x2", ct, RawV(64), 0);
        }
    }
    // Atomic INC/DEC semantics with a small DATA (GCN wraps at DATA, not at 2^32). Not a bounds
    // property; reported in its own group.
    for (auto [name, op, at] : {std::tuple{"buffer_atomic_inc", BUFFER_ATOMIC_INC, GCN_AT_INC},
                                std::tuple{"buffer_atomic_dec", BUFFER_ATOMIC_DEC, GCN_AT_DEC}}) {
        const u32 t = AtomicOp(name, op, at, false, ret);
        Job& j = NewJob("atomic-semantics", t, RawV(64), 0);
        SetLane(j, 0, 0, 8, {5, 0, 0, 0});
    }
}

// Wave64 lane semantics: the PS4 runs 64-lane waves; Apple subgroups are 32 wide.
void AddWaveCases() {
    auto rotl = [](u64 x, u32 r) { return r ? (x << r) | (x >> (64 - r)) : x; };
    auto value = [](u32 l) { return 0x1000u + l * 0x11u; };
    struct M { const char* name; u64 mask; };
    const M masks[] = {{"all", ~0ull}, {"none", 0}, {"lane63", 1ull << 63}, {"upper32", 0xFFFFFFFF00000000ull},
                       {"lower32", 0xFFFFFFFFull}, {"even", 0x5555555555555555ull}, {"lanes0+37", 1ull | 1ull << 37},
                       {"lane33", 1ull << 33}, {"lanes5+40", 1ull << 5 | 1ull << 40}};
    const std::array<u32, 3> shapes[] = {{64, 1, 1}, {8, 8, 1}, {16, 4, 1}, {2, 32, 1}, {128, 1, 1}, {32, 4, 1}};
    for (const auto& wg : shapes) {
        const std::string shape = " [" + std::to_string(wg[0]) + "x" + std::to_string(wg[1]) + "]";
        auto tmpl = [&](const char* name, Wave w) {
            Template t;
            t.name = std::string(name) + shape;
            t.op.kind = GCN_LOAD;
            t.wave = w;
            t.wg = wg;
            // cross-lane ops inside an s_and_saveexec scope: divergent, loud by design on Apple
            t.wave_divergent = w == Wave::Exec1 || w == Wave::Exec2;
            return AddTemplate(t);
        };
        const u32 threads = wg[0] * wg[1] * wg[2];
        const u32 lane = tmpl("wave: v_mbcnt(-1) lane id + VOPC ballot + s_bcnt1", Wave::Lane);
        const u32 mbcnt = tmpl("wave: v_mbcnt over VCC + s_mov_b64 vcc + v_cndmask", Wave::MbcntMask);
        const u32 rl = tmpl("wave: v_readlane 37/s13/s12 + v_readfirstlane", Wave::Readlane);
        const u32 e1 = tmpl("wave: under saveexec: v_readfirstlane, s_bcnt1 exec, masked v_mov", Wave::Exec1);
        const u32 e2 = tmpl("wave: under saveexec: s_mov_b64 exec, v_mbcnt over exec", Wave::Exec2);
        const bool all_masks = wg[0] == 64; // full mask matrix on 64x1, a subset on the other shapes
        for (const M& m : masks) {
            if (!all_masks && std::string(m.name) != "all" && std::string(m.name) != "lanes5+40" &&
                std::string(m.name) != "upper32") {
                continue;
            }
            auto bit = [&](u32 l) { return u32((l < 64 ? m.mask : rotl(m.mask, 7)) >> (l % 64) & 1); };
            for (u32 t : {lane, mbcnt}) {
                Job& j = NewJob("wave64", t, RawV(64), 0);
                j.label += " ballot=" + std::string(m.name);
                for (u32 l = 0; l < threads; ++l) SetLane(j, l, value(l), 0, {bit(l), 0, 0, 0});
            }
            if (m.mask == 0) continue; // an all-zero exec skips the scope
            for (u32 t : {e1, e2}) {
                Job& j = NewJob("wave64", t, RawV(64), 0);
                j.label += " exec=" + std::string(m.name);
                for (u32 l = 0; l < threads; ++l) {
                    SetLane(j, l, value(l), 0);
                    j.lanes[l].active = bit(l);
                }
            }
        }
        for (auto [a, b] : {std::pair{0u, 31u}, std::pair{32u, 63u}, std::pair{45u, 1u}, std::pair{63u, 32u}}) {
            if (!all_masks && a != 45) continue;
            Job& j = NewJob("wave64", rl, RawV(64), 0, b);
            j.smem_sgpr = a;
            j.label += " s13=" + std::to_string(a) + " s12=" + std::to_string(b);
            for (u32 l = 0; l < threads; ++l) SetLane(j, l, value(l), 0);
        }
    }
    // Loop-carried divergence: one wave -> exact; two waves -> the loop exit differs per wave, so a
    // ballot in the loop body cannot be exchanged between barriers (loud by design).
    for (const std::array<u32, 3>& wg : {std::array<u32, 3>{64, 1, 1}, std::array<u32, 3>{128, 1, 1}}) {
        Template t;
        t.name = "wave: loop counter += v_readfirstlane, ballot in the loop [" + std::to_string(wg[0]) + "x1]";
        t.op.kind = GCN_LOAD;
        t.wave = Wave::LoopReadFirstLane;
        t.wg = wg;
        t.wave_divergent = wg[0] > 64;
        const u32 tt = AddTemplate(t);
        Job& j = NewJob("wave64", tt, RawV(64), 0);
        for (u32 l = 0; l < wg[0]; ++l) SetLane(j, l, 0x1000u + l * 0x11u, 0, {u32(l % 3 == 0), 0, 0, 0});
    }
    // Guest LDS widths next to the wave scratch: 32-bit is exact; 16/64-bit would need a second
    // workgroup type (explicit layout), a named loud rejection on Apple.
    for (u32 bits : {32u, 16u, 64u}) {
        Template t;
        t.name = "wave: guest LDS " + std::to_string(bits) + "-bit + VOPC ballot";
        t.op.kind = GCN_LOAD;
        t.wave = Wave::Lds;
        t.lds_bits = bits;
        t.wave_divergent = bits != 32;
        const u32 tt = AddTemplate(t);
        Job& j = NewJob("wave64", tt, RawV(64), 0);
        for (u32 l = 0; l < 64; ++l) SetLane(j, l, 0x51000u + l * 0x101u, 0xABC00u + l, {u32(l % 5 == 1), 0, 0, 0});
    }
    // Control-dependent phi feeding a loop bound (Sol-2's review case), compile-only.
    for (u32 x : {64u, 128u}) {
#ifndef BB_MIRROR_PROFILE_WAVE64
        if (x > 64) continue; // the rejection is a property of the wave64 emulation
#endif
        Template t;
        t.name = "wave: branch on readfirstlane(LII) -> phi(1,2) loop bound, ballot in loop [" +
                 std::to_string(x) + "x1] (compile)";
        t.op.kind = GCN_LOAD;
        t.wave = Wave::CtrlPhi;
        t.compile_only = true;
        t.wg = {x, 1, 1};
        t.expect_loud = x > 64;
        const u32 tt = AddTemplate(t);
        NewJob("wave64", tt, RawV(64), 0);
    }
    // add_tid on a non-linear shape: thread_id[5:0] is the wave lane of LocalInvocationIndex
    {
        Template t = Base("buffer_load_dword", GCN_LOAD, GcnAsm::BUFFER_LOAD_DWORD, TArgs{.idxen = true, .offen = true});
        t.name += " [8x8]";
        t.op.comp_bytes = 4;
        t.op.ncomp = 1;
        t.wg = {8, 8, 1};
        const u32 tt = AddTemplate(t);
        gcn_vsharp v = StructV(16, 40);
        v.add_tid = 1;
        Job& j = NewJob("add-tid", tt, v, 0);
        for (u32 l = 0; l < 64; ++l) SetLane(j, l, l % 3 == 0 ? 0 : 1, (l % 5) * 4);
    }
}

// Vertex fetch: indexed draws whose indices run past num_records. GCN fetch shaders are
// BUFFER/TBUFFER_LOAD_FORMAT idxen loads, so out-of-range vertices read 0 (§8.1.5.3).
void AddVertexCases() {
    auto tmpl = [](const char* name, bool mtbuf, u32 dfmt, u32 nfmt) {
        Template t;
        t.name = name;
        t.vertex = true;
        t.mtbuf = mtbuf;
        t.idxen = true;
        t.offen = false;
        t.op.kind = GCN_LOAD;
        t.op.typed = 1;
        t.op.inst_typed = mtbuf;
        t.op.inst_dfmt = dfmt;
        t.op.inst_nfmt = nfmt;
        t.op.ncomp = 4;
        return AddTemplate(t);
    };
    const u32 fmt = tmpl("vertex: fetch buffer_load_format_xyzw v0 idxen", false, 0, 0);
    const u32 tb = tmpl("vertex: fetch tbuffer_load_format_xyzw dfmt:14 nfmt:4", true, GCN_DFMT_32_32_32_32, GCN_NFMT_UINT);
    gcn_vsharp a = StructV(16, 5, GCN_DFMT_32_32_32_32, GCN_NFMT_UINT);
    gcn_vsharp b = StructV(8, 3, GCN_DFMT_32_32, GCN_NFMT_FLOAT);
    b.dst_sel[2] = 0; b.dst_sel[3] = 1;
    gcn_vsharp c = StructV(4, 6, GCN_DFMT_8_8_8_8, GCN_NFMT_UINT);
    gcn_vsharp d = StructV(12, 4, GCN_DFMT_32_32_32, GCN_NFMT_UINT);
    d.dst_sel[3] = 1;
    gcn_vsharp e = StructV(16, 0, GCN_DFMT_32_32_32_32, GCN_NFMT_UINT);
    gcn_vsharp f = StructV(20, 3, GCN_DFMT_32_32_32_32, GCN_NFMT_UINT); // stride > element
    struct VC { u32 t; gcn_vsharp v; u64 mis; };
    for (const VC& vc : {VC{fmt, a, 0}, VC{fmt, a, 4}, VC{fmt, b, 0}, VC{fmt, c, 0}, VC{fmt, d, 0}, VC{fmt, e, 0},
                         VC{fmt, f, 0}, VC{tb, a, 0}, VC{tb, StructV(16, 5), 0}}) {
        Job& j = NewJob("vertex-fetch", vc.t, vc.v, vc.mis);
        // slot = index & 63: small indices 0..nr+3, then huge indices on slots 40..43
        for (u32 i = 0; i < vc.v.num_records + 4 && i < 40; ++i) SetLane(j, i, i, 0);
        const u32 big[] = {0x10000000u, 0x7FFFFFC0u, 0xFFFFFF80u, 0x40000000u};
        for (u32 k = 0; k < 4; ++k) SetLane(j, 40 + k, big[k] | (40 + k), 0);
    }
    // indirect draws: the CP passes the base vertex in s13 and the fetch shader adds it to v0
    {
        Template ti = g_templates[fmt];
        ti.name = "vertex: indirect, fetch v_add_i32 v0, s13, v0 + buffer_load_format_xyzw";
        ti.indirect = true;
        ti.code.clear();
        ti.fetch.clear();
        const u32 ind = AddTemplate(ti);
        for (s32 off : {0, 64, 0x0FFFFFC0}) {
            for (const gcn_vsharp& v : {a, d}) {
                Job& j = NewJob("vertex-fetch", ind, v, 0);
                j.vertex_offset = off;
                j.label += " indirect vertexOffset=" + Hex(u32(off));
                for (u32 i = 0; i < v.num_records + 4; ++i) SetLane(j, i, u32(off) + i, 0);
                SetLane(j, 40, u32(off) + 0x10000000u + 40, 0);
            }
        }
    }
    // direct draws with a base vertex: the fetch index is index + vertexOffset
    for (s32 off : {64, 0x0FFFFFC0}) {
        Job& j = NewJob("vertex-fetch", fmt, a, 0);
        j.vertex_offset = off;
        j.label += " vertexOffset=" + Hex(u32(off));
        for (u32 i = 0; i < 9; ++i) SetLane(j, i, u32(off) + i, 0);
        for (u32 i = 0; i < 4 && off == 64; ++i) SetLane(j, 60 + i, u32(off) + 60 + i - 64, 0); // negative-ish range
    }
}

// Internal buffer reads the bounds path relies on: V#s and constants from extended user data
// (s_load through a pointer in s[2:3], read back from the flattened user-data buffer), and graphics
// stages whose only buffer access is s_buffer_load (no other resource that creates the flatbuf).
void AddInternalCases() {
    using namespace GcnAsm;
    auto srt = [](const char* name, u32 opcode, bool idxen) {
        Template t = Base(std::string("srt: ") + name, GCN_LOAD, opcode, TArgs{.idxen = idxen, .offen = true});
        t.op.comp_bytes = 4;
        t.op.ncomp = 1;
        t.srt = true;
        return AddTemplate(t);
    };
    const u32 raw = srt("s_load_dwordx4 V# + s_load_dword const + buffer_load_dword", BUFFER_LOAD_DWORD, false);
    const u32 st = srt("s_load_dwordx4 V# + s_load_dword const + buffer_load_dword", BUFFER_LOAD_DWORD, true);
    for (u32 nr : {8u, 64u}) {
        Job& j = NewJob("srt", raw, RawV(nr), 0);
        const auto offs = Uniq(RawProbe(nr));
        for (u32 i = 0; i < offs.size(); ++i) SetLane(j, i, 0, offs[i]);
    }
    for (u32 nr : {4u, 1u}) {
        Job& j = NewJob("srt", st, StructV(16, nr), 0);
        u32 lane = 0;
        for (u32 i : {0u, nr - 1, nr, 0x10000001u}) for (u32 o : {0u, 12u, 16u}) SetLane(j, lane++, i, o);
    }
    auto stage = [](bool fragment, u32 count, u32 sdst, SmemSrc src, u32 imm, bool dead = false) {
        Template t;
        t.name = std::string(dead ? "dead " : "") + (fragment ? "fs" : "vs") + ": s_buffer_load_dword" +
                 (count > 1 ? "x" + std::to_string(count) : "") + " s[" + std::to_string(sdst) + "], s[4:7]" +
                 (src == SmemSrc::Imm ? " imm:" + std::to_string(imm) : " soff:s9") + " only (compile)";
        t.compile_only = true;
        t.fragment = fragment;
        t.vertex = !fragment;
        t.op.kind = GCN_SLOAD;
        t.op.ncomp = count;
        t.smem = src;
        t.smem_imm = imm;
        t.sdst = sdst;
        t.dead = dead;
        return AddTemplate(t);
    };
    for (bool fragment : {true, false}) {
        for (auto [count, sdst, src, imm] : {std::tuple{4u, 0u, SmemSrc::Imm, 0u}, std::tuple{4u, 16u, SmemSrc::Imm, 0u},
                                             std::tuple{1u, 0u, SmemSrc::Imm, 3u}, std::tuple{4u, 16u, SmemSrc::Sgpr, 0u}}) {
            const u32 t = stage(fragment, count, sdst, src, imm);
            for (const gcn_vsharp& v : {RawV(64), StructV(16, 4), RawV(0)}) NewJob("stage-compile", t, v, 0);
        }
        // the descriptor stays declared but every access to it is dead
        for (u32 sdst : {0u, 16u}) {
            const u32 t = stage(fragment, 4, sdst, SmemSrc::Imm, 0, true);
            for (const gcn_vsharp& v : {RawV(64), StructV(16, 4)}) NewJob("dead-access", t, v, 0);
        }
    }
    // compute: the buffer under test is loaded into a register that is never stored
    for (bool idxen : {false, true}) {
        Template t = Base("dead: buffer_load_dword into v20 (unused)", GCN_LOAD, BUFFER_LOAD_DWORD,
                          TArgs{.idxen = idxen, .offen = true});
        t.op.comp_bytes = 4;
        t.op.ncomp = 1;
        t.dead = true;
        const u32 tt = AddTemplate(t);
        for (const gcn_vsharp& v : {RawV(64), StructV(16, 4)}) {
            Job& j = NewJob("dead-access", tt, v, 0);
            for (u32 lane = 0; lane < 8; ++lane) SetLane(j, lane, lane, lane * 4);
        }
    }
}

// Specialization reuse across start bindings: the same vertex shader used after fragment
// shaders with different resource counts must get a module whose bindings start where the
// pipeline layout puts them (Backend::Bindings start), even when its only binding is special.
void AddSpecReuseCases() {
    for (bool only_special : {true, false}) {
        Template t;
        t.name = std::string("vs + user clip plane 0, ") +
                 (only_special ? "no guest buffer (ClipPlanes only)" : "s_buffer_load s[4:7] (control)") + " (compile)";
        t.compile_only = true;
        t.vertex = true;
        t.clip_planes = true;
        t.no_buffer_load = only_special;
        t.op.kind = GCN_SLOAD;
        t.op.ncomp = 4;
        t.smem = SmemSrc::Imm;
        t.sdst = 16;
        t.check_bindings = true;
        const u32 tt = AddTemplate(t);
        for (u32 start : {0u, 3u, 0u, 5u}) {
            Job& j = NewJob("spec-reuse", tt, RawV(64), 0);
            j.start_unified = j.start_buffer = start;
            j.label += " start binding " + std::to_string(start);
        }
    }
}

// Rasterized draws read back from a render target: a vertex-id full-screen triangle with no
// fetch shader, the same from fetched positions, and RectLists through the production
// tessellation emulation (aux TCS/TES), full and quarter coverage.
void AddRenderCases() {
    Template fs;
    fs.name = "render: constant-color fragment shader";
    fs.fragment = true;
    fs.helper = true;
    const u32 fs_t = AddTemplate(fs);
    struct R { const char* name; bool rect, fetch, quarter; u32 scale; };
    const R cases[] = {{"triangle list, positions from vertex id, no fetch shader", false, false, false, 246},
                       {"triangle list, positions fetched", false, true, false, 0},
                       {"RectList, positions from vertex id, full", true, false, false, 244},
                       {"RectList, positions from vertex id, quarter", true, false, true, 242},
                       {"RectList, positions fetched, quarter", true, true, true, 0}};
    for (const R& r : cases) {
        Template t;
        t.name = std::string("render: ") + r.name;
        t.vertex = true;
        t.render = true;
        t.rect_list = r.rect;
        t.vs_fetch = r.fetch;
        t.pos_scale = r.scale;
        t.fs_tmpl = fs_t;
        t.op.kind = GCN_LOAD;
        const u32 tt = AddTemplate(t);
        Job& f = NewJob("render", fs_t, RawV(64), 0);
        f.label = "fs helper";
        const gcn_vsharp v = StructV(16, 3, GCN_DFMT_32_32_32_32, GCN_NFMT_FLOAT);
        Job& j = NewJob("render", tt, v, 0);
        j.quarter = r.quarter;
        if (r.fetch) {
            const float hi = r.quarter ? 0.0f : 3.0f;
            const float pos[3][4] = {{-1, -1, 0, 1}, {hi, -1, 0, 1}, {-1, hi, 0, 1}};
            for (const auto& p4 : pos) for (float x : p4) j.init_words.push_back(std::bit_cast<u32>(x));
        }
        for (u32 l = 0; l < 3; ++l) SetLane(j, l, l, 0);
    }
}

// Documents MoltenVK: with VK_KHR_push_descriptor, OpArrayLength of a storage buffer binding
// reports the wrong size (a 36-byte range reads back as 1 byte / 0 dwords), so explicit bounds
// checks zero in-range accesses. The renderer binds Apple guest pipelines with allocated sets;
// these jobs bind with push descriptors on purpose. FAIL here is KNOWN-MOLTENVK; a PASS means
// MoltenVK fixed it.
void AddPushDescriptorCases() {
    using namespace GcnAsm;
    const u32 dw = RawOp("buffer_load_dword", GCN_LOAD, BUFFER_LOAD_DWORD, 4, 1, false, TArgs{});
    const u32 ub = RawOp("buffer_load_ubyte", GCN_LOAD, BUFFER_LOAD_UBYTE, 1, 1, false, TArgs{});
    for (u32 t : {dw, ub}) {
        for (u32 nr : {36u, 64u, 7u}) {
            Job& j = NewJob("moltenvk-push", t, RawV(nr), 0);
            j.force_push = true;
            j.label += " (push descriptors)";
            const auto offs = Uniq({0, 4, nr >= 4 ? nr - 4ull : 0, nr - 1ull, nr, nr + 4ull});
            for (u32 i = 0; i < offs.size(); ++i) SetLane(j, i, 0, offs[i]);
        }
    }
}

// Shader reuse: StageSpecialization decides whether a compiled module is reused for a new V#.
// Fields outside the key (num_records, add_tid, base) must be read at run time.
void AddReuseCases() {
    using namespace GcnAsm;
    const u32 t = RawOp("buffer_load_dword", GCN_LOAD, BUFFER_LOAD_DWORD, 4, 1, false,
                        TArgs{.idxen = true, .offen = false, .inst_offset = 0});
    for (u32 nr : {64u, 3u, 40u}) {
        for (bool tid : {false, true}) {
            gcn_vsharp v = StructV(16, nr);
            v.add_tid = tid;
            Job& j = NewJob("reuse", t, v, 0);
            for (u32 lane = 0; lane < 64; ++lane) SetLane(j, lane, lane & 1 ? 2 : 0, 0);
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Device capabilities (queried in a child before any compile, so the parent stays single
// threaded until the Vulkan phase).

struct DeviceCaps {
    u32 api, subgroup_size, storage_align;
    u32 int8, int16, int64, float16, float64, rba, rba2, null_descriptor, buffer_int64_atomics;
    u32 max_shared;
    u32 min_subgroup, max_subgroup, required_size_stages, size_control, full_subgroups;
    u32 subgroup_stages, subgroup_ops;
    u32 explicit_layout; // VK_KHR_workgroup_memory_explicit_layout
};

vk::UniqueInstance CreateInstance() {
    vk::detail::DynamicLoader loader;
    VULKAN_HPP_DEFAULT_DISPATCHER.init(loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr"));
    static vk::detail::DynamicLoader keep; // keep libvulkan loaded
    const vk::ApplicationInfo app{.pApplicationName = "gcn buffer conformance", .apiVersion = VK_API_VERSION_1_3};
    const char* ext = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
    auto instance = vk::createInstanceUnique({.flags = vk::InstanceCreateFlagBits::eEnumeratePortabilityKHR,
                                              .pApplicationInfo = &app,
                                              .enabledExtensionCount = 1,
                                              .ppEnabledExtensionNames = &ext});
    if (instance.result != vk::Result::eSuccess) {
        std::fprintf(stderr, "gcn-conformance: vkCreateInstance failed: %s\n", vk::to_string(instance.result).c_str());
        std::exit(2);
    }
    VULKAN_HPP_DEFAULT_DISPATCHER.init(*instance.value);
    return std::move(instance.value);
}

DeviceCaps QueryCaps() {
    auto instance = CreateInstance();
    auto physical = instance->enumeratePhysicalDevices().value.front();
    auto chain = physical.getFeatures2<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceVulkan11Features,
                                       vk::PhysicalDeviceVulkan12Features, vk::PhysicalDeviceRobustness2FeaturesEXT,
                                       vk::PhysicalDeviceVulkan13Features>();
    auto props = physical.getProperties2<vk::PhysicalDeviceProperties2, vk::PhysicalDeviceSubgroupProperties,
                                         vk::PhysicalDeviceVulkan13Properties>();
    const auto& f = chain.get<vk::PhysicalDeviceFeatures2>().features;
    const auto& f12 = chain.get<vk::PhysicalDeviceVulkan12Features>();
    const auto& r2 = chain.get<vk::PhysicalDeviceRobustness2FeaturesEXT>();
    const auto& p = props.get<vk::PhysicalDeviceProperties2>().properties;
    DeviceCaps c{};
    c.api = p.apiVersion;
    c.subgroup_size = props.get<vk::PhysicalDeviceSubgroupProperties>().subgroupSize;
    c.storage_align = u32(p.limits.minStorageBufferOffsetAlignment);
    c.int8 = f12.shaderInt8;
    c.int16 = f.shaderInt16;
    c.int64 = f.shaderInt64;
    c.float16 = f12.shaderFloat16;
    c.float64 = f.shaderFloat64;
    c.rba = f.robustBufferAccess;
    c.rba2 = r2.robustBufferAccess2;
    c.null_descriptor = r2.nullDescriptor;
    c.buffer_int64_atomics = f12.shaderBufferInt64Atomics;
    c.max_shared = p.limits.maxComputeSharedMemorySize;
    const auto& p13 = props.get<vk::PhysicalDeviceVulkan13Properties>();
    const auto& f13 = chain.get<vk::PhysicalDeviceVulkan13Features>();
    const auto& sg = props.get<vk::PhysicalDeviceSubgroupProperties>();
    c.min_subgroup = p13.minSubgroupSize;
    c.max_subgroup = p13.maxSubgroupSize;
    c.required_size_stages = u32(p13.requiredSubgroupSizeStages);
    c.size_control = f13.subgroupSizeControl;
    c.full_subgroups = f13.computeFullSubgroups;
    c.subgroup_stages = u32(sg.supportedStages);
    c.subgroup_ops = u32(sg.supportedOperations);
    for (const auto& e : physical.enumerateDeviceExtensionProperties().value) {
        if (std::strcmp(e.extensionName, "VK_KHR_workgroup_memory_explicit_layout") == 0) c.explicit_layout = 1;
    }
    return c;
}

// ---------------------------------------------------------------------------------------------
// Compile phase (child processes). Mirrors PipelineCache::GetProgram / CompileModule.

Shader::Profile g_profile;
DeviceCaps g_caps;

Shader::Profile MakeProfile(const DeviceCaps& c) {
    // Same fields vk_pipeline_cache.cpp fills from the Instance; float-control and other
    // properties that do not affect buffer lowering stay default.
    Shader::Profile p{};
    p.max_viewport_width = 16384;
    p.max_viewport_height = 16384;
    p.max_shared_memory_size = c.max_shared;
    p.supported_spirv = 0x00010600;
    p.subgroup_size = c.subgroup_size;
    p.support_int8 = c.int8;
    p.support_int16 = c.int16;
    p.support_int64 = c.int64;
    p.support_float16 = c.float16;
    p.support_float64 = c.float64;
    p.supports_buffer_int64_atomics = c.buffer_int64_atomics;
    p.needs_buffer_offsets = c.storage_align > 4;
    p.emulate_null_descriptors = !c.null_descriptor;
    p.supports_workgroup_explicit_memory_layout = c.explicit_layout;
    // Apple-only Profile switches: set exactly when the tree's PipelineCache sets them on Apple
    // (build.sh detects each assignment), so the suite tests what the renderer would compile.
    [&](auto& prof) {
#ifdef BB_MIRROR_PROFILE_ROBUST
        if constexpr (requires { prof.emulate_robust_buffer_access; }) prof.emulate_robust_buffer_access = true;
#endif
#ifdef BB_MIRROR_PROFILE_WAVE64
        if constexpr (requires { prof.emulate_wave64; }) prof.emulate_wave64 = true;
#endif
#ifdef BB_MIRROR_PROFILE_VERTEX_FETCH
        if constexpr (requires { prof.emulate_vertex_fetch; }) prof.emulate_vertex_fetch = true;
#endif
#ifdef BB_MIRROR_PROFILE_SIGNED_ZERO
        if constexpr (requires { prof.repair_mul_signed_zero; }) prof.repair_mul_signed_zero = true;
#endif
    }(p);
    return p;
}

bool VertexPulling() {
    return [](auto& prof) {
        if constexpr (requires { prof.emulate_vertex_fetch; }) return bool(prof.emulate_vertex_fetch);
        else return false;
    }(g_profile);
}

bool ProfileHasRobustFlag() {
    return [](auto& prof) {
        if constexpr (requires { prof.emulate_robust_buffer_access; }) return true;
        else return false;
    }(g_profile);
}

u32 RenderFsInputs() {
    const char* env = std::getenv("BB_RENDER_FS_INPUTS");
    return env ? u32(std::atoi(env)) : 0;
}

u32 g_fs_export = 9; // MRT0 export format for fragment compiles (9 = ABGR_32; 4 = ABGR_FP16 for compressed exports)

Shader::RuntimeInfo MakeRuntime(bool vertex = false, bool fragment = false, bool indirect = false,
                                std::array<u32, 3> wg = {64, 1, 1}, bool clip_planes = false,
                                u32 lds_bytes = 0, bool tess_rect = false, u32 fs_inputs = 1) {
    Shader::RuntimeInfo rt{};
    if (fragment) {
        rt.Initialize(Shader::HwStage::Fragment, Shader::SwStage::Fragment);
        rt.props.num_user_data = 16;
        rt.props.num_allocated_vgprs = 64;
        rt.hw.fs.en_flags.persp_center_ena = 1;
        rt.hw.fs.addr_flags.persp_center_ena = 1;
        rt.hw.fs.num_inputs = fs_inputs;
        rt.hw.fs.inputs[0].param_index = 0;
        rt.hw.fs.color_buffers[0].export_format = AmdGpu::ShaderExportFormat(g_fs_export);
        rt.hw.fs.color_buffers[0].data_format = AmdGpu::DataFormat::Format32_32_32_32;
        rt.hw.fs.color_buffers[0].num_format = AmdGpu::NumberFormat::Float;
        rt.hw.fs.color_buffers[0].swizzle = AmdGpu::CompMapping{{AmdGpu::CompSwizzle::Red, AmdGpu::CompSwizzle::Green,
                                                                 AmdGpu::CompSwizzle::Blue, AmdGpu::CompSwizzle::Alpha}};
        return rt;
    }
    if (vertex) {
        rt.Initialize(Shader::HwStage::Vertex, Shader::SwStage::Vertex);
        rt.props.num_user_data = 16;
        rt.props.num_allocated_vgprs = 64;
        if (clip_planes) {
            rt.hw.vs.user_clip_plane_mask = 1;
        }
        rt.sw.vs.tess_emulated_primitive = tess_rect;
        if (indirect) {
            // DrawIndirectParams: SH offset of the SGPR the CP writes the base vertex into
            // (translate.cpp UdRegFromShOffset: vertex stage user data start at 0x4c).
            rt.sw.vs.vertex_sgpr_offset = 0x4c + IndirectBaseSgpr;
        }
        return rt;
    }
    rt.Initialize(Shader::HwStage::Compute, Shader::SwStage::Compute);
    rt.props.num_user_data = 16;
    rt.props.num_allocated_vgprs = 64;
    rt.hw.cs.workgroup_size = wg;
    rt.hw.cs.shared_memory_size = lds_bytes;
    rt.hw.cs.tgid_enable = {false, false, false};
    return rt;
}

struct ProgramState {
    std::unique_ptr<Shader::Info> info;
    std::vector<Shader::StageSpecialization> specs;
};

struct Writer {
    std::vector<u32> buf;
    void Put(u32 x) { buf.push_back(x); }
    void Put(const std::vector<u32>& v) {
        Put(u32(v.size()));
        buf.insert(buf.end(), v.begin(), v.end());
    }
};

// RectList aux tessellation shaders. Trees with the VS-output-location argument get 0: the render
// VS writes position only.
template <typename Fs>
std::vector<u32> AuxTess(Shader::Backend::SPIRV::AuxShaderType type, const Fs& fs) {
    if constexpr (requires { Shader::Backend::SPIRV::EmitAuxilaryTessShader(type, fs, 0u); }) {
        return Shader::Backend::SPIRV::EmitAuxilaryTessShader(type, fs, 0u);
    } else {
        return Shader::Backend::SPIRV::EmitAuxilaryTessShader(type, fs);
    }
}

constexpr u32 RecordMagic = 0x4A4F4231;

void CompileJob(std::vector<ProgramState>& programs, Shader::Pools& pools, u32 index, int fd) {
    Job& job = g_jobs[index];
    const Template& t = g_templates[job.tmpl];
    Shader::Profile prof = g_profile;
    if (t.force_int64_atomics) prof.supports_buffer_int64_atomics = true; // compile-only capability check
    ProgramState& ps = programs[job.tmpl];
    const Shader::ShaderParams params{.user_data = std::span<const u32, 16>(job.ud),
                                      .code = std::span<const u32>(t.code),
                                      .hash = t.hash};
    auto rt = MakeRuntime(t.vertex, t.fragment, t.indirect, t.wg, t.clip_planes, t.Threads() * t.lds_bits / 8,
                          t.rect_list, t.helper ? RenderFsInputs() : 1);
    const auto hw = t.fragment ? Shader::HwStage::Fragment
                               : (t.vertex ? Shader::HwStage::Vertex : Shader::HwStage::Compute);
    const auto sw = t.fragment ? Shader::SwStage::Fragment
                               : (t.vertex ? Shader::SwStage::Vertex : Shader::SwStage::Compute);
    Shader::Backend::Bindings binding{.unified = job.start_unified, .buffer = job.start_buffer};
    std::vector<u32> spv;
    u32 perm = 0;
    if (!ps.info) {
        ps.info = std::make_unique<Shader::Info>(hw, sw, params);
        const auto start = binding;
        const auto program = Shader::TranslateProgram(t.code, pools, *ps.info, rt, prof);
        spv = Shader::Backend::SPIRV::EmitSPIRV(prof, rt, program, binding);
        ps.specs.emplace_back(*ps.info, rt, prof, start);
    } else {
        auto& info = *ps.info;
        info.pgm_base = params.Base();
        info.user_data = params.user_data;
        info.RefreshFlatBuf();
        Shader::StageSpecialization spec(info, rt, prof, binding);
        const auto it = std::ranges::find(ps.specs, spec);
        if (it != ps.specs.end()) {
            perm = u32(std::distance(ps.specs.begin(), it));
        } else {
            perm = u32(ps.specs.size());
            Shader::Info new_info(hw, sw, params);
            const auto program = Shader::TranslateProgram(t.code, pools, new_info, rt, prof);
            spv = Shader::Backend::SPIRV::EmitSPIRV(prof, rt, program, binding);
            ps.specs.push_back(std::move(spec));
        }
    }
    if (fd < 0) return;
    // Binding plan from program->info, as Rasterizer::BindBuffers sees it for this draw.
    auto& info = *ps.info;
    info.pgm_base = params.Base();
    info.user_data = params.user_data;
    info.RefreshFlatBuf();
    Writer w;
    w.Put(RecordMagic);
    w.Put(index);
    w.Put(perm);
    w.Put(spv);
    w.Put(u32(info.buffers.size()));
    for (const auto& desc : info.buffers) {
        w.Put(u32(desc.buffer_type));
        const u64 required = [](const auto& d) -> u64 {
            if constexpr (requires { d.required_binding_bytes; }) return d.required_binding_bytes;
            else return 0;
        }(desc);
        w.Put(u32(required));
        w.Put(u32(required >> 32));
        std::vector<u32> payload;
        if (desc.buffer_type == Shader::BufferType::Guest) {
            const auto sharp = desc.GetSharp(info);
            payload.resize(4);
            std::memcpy(payload.data(), &sharp, 16);
        } else if (desc.buffer_type == Shader::BufferType::Flatbuf) {
            const auto flat = info.FlatUserData();
            payload.assign(flat.begin(), flat.end());
        }
        w.Put(payload);
    }
    if (!info.images.empty() || !info.samplers.empty()) {
        w.Put(u32(1)); // unexpected resources
    } else {
        w.Put(u32(0));
    }
    Shader::PushData push{};
    Shader::Backend::Bindings ud_binding{};
    info.PushUd(ud_binding, push);
    for (u32 i = 0; i < 16; ++i) w.Put(push.ud_regs[i]);
    // Vertex inputs the graphics pipeline would declare (GraphicsPipeline::GetVertexInputs).
    const auto& fetch = ps.specs[perm].fetch_shader_data;
    const u32 nattr = (t.vertex && fetch) ? u32(fetch->attributes.size()) : 0;
    w.Put(nattr);
    for (u32 a = 0; a < nattr; ++a) {
        const auto& attrib = fetch->attributes[a];
        const auto sharp = attrib.GetSharp(info);
        w.Put(u32(attrib.semantic));
        w.Put(u32(Vulkan::LiverpoolToVK::SurfaceFormat(sharp.GetDataFmt(), sharp.GetNumberFmt())));
        u32 dw[4];
        std::memcpy(dw, &sharp, 16);
        for (u32 x : dw) w.Put(x);
    }
    const u8* p = reinterpret_cast<const u8*>(w.buf.data());
    size_t left = w.buf.size() * 4;
    while (left) {
        const ssize_t n = write(fd, p, left);
        if (n <= 0) _exit(90);
        p += n;
        left -= size_t(n);
    }
}

[[noreturn]] void CompileChild(const std::vector<u32>& replay, u32 first, int fd, const std::string& log) {
    const int lfd = open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (lfd >= 0) {
        dup2(lfd, 1);
        dup2(lfd, 2);
    }
    std::vector<ProgramState> programs(g_templates.size());
    static Shader::Pools pools;
    for (u32 j : replay) CompileJob(programs, pools, j, -1);
    for (u32 j = first; j < g_jobs.size(); ++j) {
        std::fprintf(stderr, "== compile job %u: %s\n", j, g_jobs[j].label.c_str());
        std::fflush(stderr);
        CompileJob(programs, pools, j, fd);
    }
    std::fflush(nullptr);
    _exit(0);
}

std::string Tail(const std::string& path, size_t max = 1500) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return {};
    std::string s;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
    std::fclose(f);
    // drop our own progress lines
    std::string out;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t e = s.find('\n', pos);
        if (e == std::string::npos) e = s.size();
        const std::string line = s.substr(pos, e - pos);
        if (line.rfind("== compile job", 0) != 0) out += line + "\n";
        pos = e + 1;
    }
    // Lead with the GPU library's assertion block when there is one.
    const size_t crit = out.find("<Critical>");
    if (crit != std::string::npos) {
        const size_t start = out.rfind('\n', crit);
        out = out.substr(start == std::string::npos ? 0 : start + 1);
    }
    return out.size() > max ? out.substr(0, max) : out;
}

std::map<std::pair<u32, u32>, std::vector<u32>> g_spirv; // (template, perm) -> SPIR-V
std::map<std::pair<u32, u32>, std::string> g_spirv_invalid; // spirv-val findings

// Validates every module with spirv-val (a native tool, so it adds no Rosetta process).
void ValidateModules(const std::string& outdir) {
    const char* env = std::getenv("BB_SPIRV_VAL");
    const std::string tool = env ? env : "/opt/homebrew/bin/spirv-val";
    if (access(tool.c_str(), X_OK) != 0) {
        std::printf("spirv-val: not found at %s (set BB_SPIRV_VAL); modules NOT validated\n", tool.c_str());
        return;
    }
    const std::string path = outdir + "/validate.spv";
    for (const auto& [key, spv] : g_spirv) {
        FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) continue;
        std::fwrite(spv.data(), 4, spv.size(), f);
        std::fclose(f);
        const std::string cmd = tool + " --target-env vulkan1.3 '" + path + "' 2>&1";
        std::fflush(nullptr);
        FILE* p = popen(cmd.c_str(), "r");
        std::string out;
        char buf[512];
        while (p && std::fgets(buf, sizeof(buf), p)) out += buf;
        const int rc = p ? pclose(p) : -1;
        if (rc != 0) g_spirv_invalid[key] = out.substr(0, out.find('\n'));
    }
    std::remove(path.c_str());
    std::printf("spirv-val: %zu/%zu modules invalid\n", g_spirv_invalid.size(), g_spirv.size());
}

void CompileAll(const std::string& outdir) {
    std::vector<u32> replay; // jobs that created a program or permutation
    u32 first = 0;
    u32 attempt = 0;
    while (first < g_jobs.size()) {
        int fds[2];
        if (pipe(fds) != 0) { perror("pipe"); std::exit(2); }
        const std::string log = outdir + "/compile-" + std::to_string(attempt++) + ".log";
        std::fflush(nullptr);
        const pid_t pid = fork();
        if (pid == 0) {
            close(fds[0]);
            CompileChild(replay, first, fds[1], log);
        }
        close(fds[1]);
        std::vector<u8> bytes;
        u8 buf[1 << 16];
        ssize_t n;
        while ((n = read(fds[0], buf, sizeof(buf))) > 0) bytes.insert(bytes.end(), buf, buf + n);
        close(fds[0]);
        int status = 0;
        waitpid(pid, &status, 0);
        // parse records
        std::vector<u32> w(bytes.size() / 4);
        std::memcpy(w.data(), bytes.data(), w.size() * 4);
        size_t pos = 0;
        u32 next = first;
        auto need = [&](size_t k) { return pos + k <= w.size(); };
        while (need(4) && w[pos] == RecordMagic) {
            size_t p = pos + 1;
            auto get = [&]() -> std::optional<u32> { if (p < w.size()) return w[p++]; return std::nullopt; };
            auto idx = get(), perm = get(), nspv = get();
            if (!idx || !perm || !nspv || p + *nspv > w.size()) break;
            std::vector<u32> spv(w.begin() + p, w.begin() + p + *nspv);
            p += *nspv;
            auto nb = get();
            if (!nb) break;
            std::vector<BindPlan> binds;
            bool ok = true;
            for (u32 b = 0; b < *nb && ok; ++b) {
                auto type = get(), req_lo = get(), req_hi = get(), cnt = get();
                if (!type || !req_lo || !req_hi || !cnt || p + *cnt > w.size()) { ok = false; break; }
                BindPlan bp;
                bp.type = *type;
                bp.required = *req_lo | u64(*req_hi) << 32;
                if (*type == u32(Shader::BufferType::Guest) && *cnt == 4) {
                    std::copy_n(w.begin() + p, 4, bp.sharp.begin());
                } else {
                    bp.flat.assign(w.begin() + p, w.begin() + p + *cnt);
                }
                p += *cnt;
                binds.push_back(std::move(bp));
            }
            auto other = get();
            if (!ok || !other || p + 16 > w.size()) break;
            Job& job = g_jobs[*idx];
            job.perm = *perm;
            job.binds = std::move(binds);
            std::copy_n(w.begin() + p, 16, job.ud_regs.begin());
            p += 16;
            if (p >= w.size()) break;
            const u32 nattr = w[p++];
            if (p + nattr * 6 > w.size()) break;
            job.vattrs.clear();
            for (u32 a = 0; a < nattr; ++a) {
                VertexAttr va;
                va.semantic = w[p];
                va.format = w[p + 1];
                std::copy_n(w.begin() + p + 2, 4, va.sharp.begin());
                job.vattrs.push_back(va);
                p += 6;
            }
            job.status = *other ? Status::HarnessError : Status::Ok;
            if (*other) job.message = "shader uses images/samplers";
            if (!spv.empty()) {
                g_spirv[{job.tmpl, job.perm}] = std::move(spv);
                replay.push_back(*idx);
            }
            pos = p;
            next = *idx + 1;
        }
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0 && next == g_jobs.size()) break;
        if (next >= g_jobs.size()) break;
        Job& bad = g_jobs[next];
        const std::string tail = Tail(log);
        // GPU library ASSERT/UNREACHABLE: gpu/shim/common/assert.cpp exits with status 23.
        const bool gpu_assert = WIFEXITED(status) && WEXITSTATUS(status) == 23;
        const bool abort_sig = WIFSIGNALED(status) && (WTERMSIG(status) == SIGABRT || WTERMSIG(status) == SIGTRAP ||
                                                       WTERMSIG(status) == SIGILL);
        if ((gpu_assert || abort_sig) && !tail.empty()) {
            bad.status = Status::LoudCompile;
        } else {
            bad.status = Status::Crash;
        }
        bad.message = (WIFSIGNALED(status) ? "signal " + std::to_string(WTERMSIG(status))
                                           : "exit " + std::to_string(WEXITSTATUS(status))) + ": " + tail;
        first = next + 1;
    }
}

// ---------------------------------------------------------------------------------------------
// Vulkan phase

// Push-descriptor state (set in InitGpu; see BindStorageBuffers).
bool g_push_supported = false, g_force_sets = true; // Apple guest pipelines: allocated sets
u32 g_max_push = 0;

struct Mem {
    vk::UniqueBuffer buffer;
    vk::UniqueDeviceMemory memory;
    u8* data = nullptr;
    u64 size = 0;
};

struct Gpu {
    vk::UniqueInstance instance;
    vk::PhysicalDevice physical;
    vk::UniqueDevice device;
    vk::Queue queue;
    u32 family = 0;
    u64 align = 16;
    vk::UniqueCommandPool pool;
    vk::UniqueDescriptorPool dpool;
    Mem guest, params, results, flat, null_buffer, index, indirect, readback;
    bool dynamic_vertex_input = false; // VK_EXT_vertex_input_dynamic_state, as the renderer uses it
    std::vector<u8> pristine; // initial guest image of the current batch
};

Mem MakeBuffer(Gpu& g, u64 size,
               vk::BufferUsageFlags usage = vk::BufferUsageFlagBits::eStorageBuffer |
                                            vk::BufferUsageFlagBits::eVertexBuffer) {
    Mem m;
    m.size = size;
    m.buffer = g.device->createBufferUnique({.size = size, .usage = usage}).value;
    const auto req = g.device->getBufferMemoryRequirements(*m.buffer);
    const auto props = g.physical.getMemoryProperties();
    const auto want = vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
    u32 type = 0;
    while (type < props.memoryTypeCount &&
           (!(req.memoryTypeBits & (1u << type)) || (props.memoryTypes[type].propertyFlags & want) != want)) {
        ++type;
    }
    assert(type < props.memoryTypeCount);
    m.memory = g.device->allocateMemoryUnique({.allocationSize = req.size, .memoryTypeIndex = type}).value;
    assert(g.device->bindBufferMemory(*m.buffer, *m.memory, 0) == vk::Result::eSuccess);
    m.data = static_cast<u8*>(g.device->mapMemory(*m.memory, 0, size).value);
    return m;
}

void InitGpu(Gpu& g, bool rba) {
    g.instance = CreateInstance();
    g.physical = g.instance->enumeratePhysicalDevices().value.front();
    const auto families = g.physical.getQueueFamilyProperties();
    while (!(families.at(g.family).queueFlags & vk::QueueFlagBits::eCompute)) ++g.family;
    // Enable every supported core feature (as the renderer enables what it uses), with
    // robustBufferAccess set like production (on when supported) unless --no-rba.
    auto chain = g.physical.getFeatures2<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceVulkan11Features,
                                         vk::PhysicalDeviceVulkan12Features, vk::PhysicalDeviceVulkan13Features>();
    chain.get<vk::PhysicalDeviceFeatures2>().features.robustBufferAccess = rba;
    std::vector<const char*> exts;
    for (const auto& e : g.physical.enumerateDeviceExtensionProperties().value) {
        if (std::strcmp(e.extensionName, "VK_KHR_portability_subset") == 0) exts.push_back("VK_KHR_portability_subset");
        if (std::strcmp(e.extensionName, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME) == 0) {
            exts.push_back(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
            g_push_supported = true;
        }
        if (std::strcmp(e.extensionName, VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME) == 0) {
            exts.push_back(VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME);
            g.dynamic_vertex_input = true;
        }
    }
    vk::PhysicalDeviceVertexInputDynamicStateFeaturesEXT dyn_vertex{.vertexInputDynamicState = true};
    if (g.dynamic_vertex_input) chain.get<vk::PhysicalDeviceVulkan13Features>().pNext = &dyn_vertex;
    const float priority = 1.0f;
    const vk::DeviceQueueCreateInfo queue_info{.queueFamilyIndex = g.family, .queueCount = 1, .pQueuePriorities = &priority};
    auto device = g.physical.createDeviceUnique({.pNext = &chain.get<vk::PhysicalDeviceFeatures2>(),
                                                 .queueCreateInfoCount = 1,
                                                 .pQueueCreateInfos = &queue_info,
                                                 .enabledExtensionCount = u32(exts.size()),
                                                 .ppEnabledExtensionNames = exts.data()});
    if (device.result != vk::Result::eSuccess) {
        std::fprintf(stderr, "gcn-conformance: vkCreateDevice failed: %s\n", vk::to_string(device.result).c_str());
        std::exit(2);
    }
    g.device = std::move(device.value);
    VULKAN_HPP_DEFAULT_DISPATCHER.init(*g.device);
    g.queue = g.device->getQueue(g.family, 0);
    g.align = g.physical.getProperties().limits.minStorageBufferOffsetAlignment;
    if (g_push_supported) {
        auto props = g.physical.getProperties2<vk::PhysicalDeviceProperties2, vk::PhysicalDevicePushDescriptorPropertiesKHR>();
        g_max_push = props.get<vk::PhysicalDevicePushDescriptorPropertiesKHR>().maxPushDescriptors;
    }
    std::printf("descriptors: %s (maxPushDescriptors %u)\n",
                g_push_supported && !g_force_sets ? "pushed (vkCmdPushDescriptorSetKHR)"
                                                  : "allocated sets (the Apple guest-pipeline path)", g_max_push);
    g.pool = g.device->createCommandPoolUnique({.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
                                                .queueFamilyIndex = g.family}).value;
    const vk::DescriptorPoolSize size{vk::DescriptorType::eStorageBuffer, MaxBatch * 8};
    g.dpool = g.device->createDescriptorPoolUnique({.maxSets = MaxBatch, .poolSizeCount = 1, .pPoolSizes = &size}).value;
    g.guest = MakeBuffer(g, u64(MaxBatch) * Arena);
    g.params = MakeBuffer(g, u64(MaxBatch) * ParamSlot);
    g.results = MakeBuffer(g, u64(MaxBatch) * ResultSlot);
    g.flat = MakeBuffer(g, u64(MaxBatch) * FlatSlot);
    g.null_buffer = MakeBuffer(g, NullBufferSize);
    g.index = MakeBuffer(g, u64(MaxBatch) * 64 * 4, vk::BufferUsageFlagBits::eIndexBuffer);
    g.indirect = MakeBuffer(g, u64(MaxBatch) * 32, vk::BufferUsageFlagBits::eIndirectBuffer);
    g.readback = MakeBuffer(g, u64(MaxBatch) * RenderBytes, vk::BufferUsageFlagBits::eTransferDst);
    std::memset(g.null_buffer.data, 0, NullBufferSize);
}

struct Pipe {
    vk::UniqueShaderModule module;
    vk::UniqueDescriptorSetLayout layout;
    vk::UniquePipelineLayout playout;
    vk::UniquePipeline pipeline;
    vk::Result result = vk::Result::eSuccess;
    bool push = false; // descriptors pushed (VK_KHR_push_descriptor) like the renderer
};
std::map<std::tuple<u32, u32, bool>, Pipe> g_pipes;

// Renderer: uses_push_descriptors = binding < instance.MaxPushDescriptors() (vk_*_pipeline.cpp),
// then cmdbuf.pushDescriptorSetKHR (vk_pipeline_common.cpp). --descriptor-sets forces allocated sets.
bool UsePushDescriptors(u32 nbind, bool force_push = false) {
    return g_push_supported && (force_push || !g_force_sets) && nbind < g_max_push;
}

void BindStorageBuffers(Gpu& g, vk::CommandBuffer cmd, vk::PipelineBindPoint bp, const Pipe& p,
                        const std::vector<vk::DescriptorBufferInfo>& infos) {
    if (infos.empty()) return;
    std::vector<vk::WriteDescriptorSet> writes;
    vk::DescriptorSet set{};
    if (!p.push) {
        const vk::DescriptorSetLayout l = *p.layout;
        set = g.device->allocateDescriptorSets({.descriptorPool = *g.dpool, .descriptorSetCount = 1, .pSetLayouts = &l})
                  .value.front();
    }
    for (u32 b = 0; b < infos.size(); ++b) {
        writes.push_back({.dstSet = set, .dstBinding = b, .descriptorCount = 1,
                          .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &infos[b]});
    }
    if (p.push) {
        cmd.pushDescriptorSetKHR(bp, *p.playout, 0, writes);
    } else {
        g.device->updateDescriptorSets(writes, {});
        cmd.bindDescriptorSets(bp, *p.playout, 0, set, {});
    }
}

Pipe& GetPipe(Gpu& g, u32 tmpl, u32 perm, u32 nbind, bool force_push = false) {
    auto [it, fresh] = g_pipes.try_emplace({tmpl, perm, force_push});
    Pipe& p = it->second;
    if (!fresh) return p;
    const auto& spv = g_spirv.at({tmpl, perm});
    auto module = g.device->createShaderModuleUnique({.codeSize = spv.size() * 4, .pCode = spv.data()});
    if (module.result != vk::Result::eSuccess) {
        p.result = module.result;
        return p;
    }
    p.module = std::move(module.value);
    std::vector<vk::DescriptorSetLayoutBinding> b;
    for (u32 i = 0; i < nbind; ++i) {
        b.push_back({.binding = i, .descriptorType = vk::DescriptorType::eStorageBuffer, .descriptorCount = 1,
                     .stageFlags = vk::ShaderStageFlagBits::eCompute});
    }
    p.push = UsePushDescriptors(nbind, force_push);
    p.layout = g.device->createDescriptorSetLayoutUnique({.flags = p.push ? vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR : vk::DescriptorSetLayoutCreateFlags{}, .bindingCount = nbind, .pBindings = b.data()}).value;
    const vk::PushConstantRange push{.stageFlags = vk::ShaderStageFlagBits::eCompute, .offset = 0,
                                     .size = sizeof(Shader::PushData)};
    const vk::DescriptorSetLayout l = *p.layout;
    p.playout = g.device->createPipelineLayoutUnique({.setLayoutCount = 1, .pSetLayouts = &l,
                                                      .pushConstantRangeCount = 1, .pPushConstantRanges = &push}).value;
    auto pipe = g.device->createComputePipelineUnique(
        {}, {.stage = {.stage = vk::ShaderStageFlagBits::eCompute, .module = *p.module, .pName = "main"},
             .layout = *p.playout});
    p.result = pipe.result;
    if (pipe.result == vk::Result::eSuccess) p.pipeline = std::move(pipe.value);
    return p;
}

// Vertex-shader-only graphics pipeline with rasterizer discard. Fixed-function vertex input
// mirrors GraphicsPipeline::GetVertexInputs (location = binding = semantic, offset 0, stride =
// V# stride), dynamic when VK_EXT_vertex_input_dynamic_state is available, as in the renderer;
// none when the tree pulls vertices in the shader (Profile::emulate_vertex_fetch).
std::map<std::string, Pipe> g_gfx_pipes;

Pipe& GetGfxPipe(Gpu& g, const Job& j, u32 nbind, bool pulling) {
    std::string key = std::to_string(j.tmpl) + "/" + std::to_string(j.perm);
    if (!pulling && !g.dynamic_vertex_input) {
        for (const auto& a : j.vattrs) {
            key += "/" + std::to_string(a.semantic) + ":" + std::to_string(a.format) + ":" +
                   std::to_string((a.sharp[1] >> 16) & 0x3FFF);
        }
    }
    auto [it, fresh] = g_gfx_pipes.try_emplace(key);
    Pipe& p = it->second;
    if (!fresh) return p;
    const auto& spv = g_spirv.at({j.tmpl, j.perm});
    auto module = g.device->createShaderModuleUnique({.codeSize = spv.size() * 4, .pCode = spv.data()});
    if (module.result != vk::Result::eSuccess) {
        p.result = module.result;
        return p;
    }
    p.module = std::move(module.value);
    std::vector<vk::DescriptorSetLayoutBinding> b;
    for (u32 i = 0; i < nbind; ++i) {
        b.push_back({.binding = i, .descriptorType = vk::DescriptorType::eStorageBuffer, .descriptorCount = 1,
                     .stageFlags = vk::ShaderStageFlagBits::eVertex});
    }
    p.push = UsePushDescriptors(nbind);
    p.layout = g.device->createDescriptorSetLayoutUnique({.flags = p.push ? vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR : vk::DescriptorSetLayoutCreateFlags{}, .bindingCount = nbind, .pBindings = b.data()}).value;
    const vk::PushConstantRange push{.stageFlags = vk::ShaderStageFlagBits::eVertex, .offset = 0,
                                     .size = sizeof(Shader::PushData)};
    const vk::DescriptorSetLayout l = *p.layout;
    p.playout = g.device->createPipelineLayoutUnique({.setLayoutCount = 1, .pSetLayouts = &l,
                                                      .pushConstantRangeCount = 1, .pPushConstantRanges = &push}).value;
    std::vector<vk::VertexInputBindingDescription> vb;
    std::vector<vk::VertexInputAttributeDescription> va;
    std::vector<vk::DynamicState> dyn;
    if (!pulling) {
        if (g.dynamic_vertex_input) {
            dyn.push_back(vk::DynamicState::eVertexInputEXT);
        } else {
            for (const auto& a : j.vattrs) {
                vb.push_back({.binding = a.semantic, .stride = (a.sharp[1] >> 16) & 0x3FFF,
                              .inputRate = vk::VertexInputRate::eVertex});
                va.push_back({.location = a.semantic, .binding = a.semantic, .format = vk::Format(a.format), .offset = 0});
            }
            dyn.push_back(vk::DynamicState::eVertexInputBindingStride);
        }
    }
    const vk::PipelineVertexInputStateCreateInfo vi{.vertexBindingDescriptionCount = u32(vb.size()),
                                                    .pVertexBindingDescriptions = vb.data(),
                                                    .vertexAttributeDescriptionCount = u32(va.size()),
                                                    .pVertexAttributeDescriptions = va.data()};
    const vk::PipelineInputAssemblyStateCreateInfo ia{.topology = vk::PrimitiveTopology::ePointList};
    const vk::PipelineRasterizationStateCreateInfo rs{.rasterizerDiscardEnable = true, .lineWidth = 1.0f};
    const vk::PipelineDynamicStateCreateInfo ds{.dynamicStateCount = u32(dyn.size()), .pDynamicStates = dyn.data()};
    const vk::PipelineRenderingCreateInfo rendering{};
    const vk::PipelineShaderStageCreateInfo stage{.stage = vk::ShaderStageFlagBits::eVertex, .module = *p.module,
                                                  .pName = "main"};
    auto pipe = g.device->createGraphicsPipelineUnique(
        {}, {.pNext = &rendering, .stageCount = 1, .pStages = &stage, .pVertexInputState = &vi,
             .pInputAssemblyState = &ia, .pRasterizationState = &rs, .pDynamicState = &ds, .layout = *p.playout});
    p.result = pipe.result;
    if (pipe.result == vk::Result::eSuccess) p.pipeline = std::move(pipe.value);
    return p;
}

u64 SlotBase(u32 slot) { return GuestVa + u64(slot) * Arena; }

// Mirror of Rasterizer::BindBuffers for one Guest V# (macOS path), against the harness regions.
struct Bound {
    vk::DescriptorBufferInfo info{};
    u32 adjust = 0;
    bool is_null = false;
    u64 lo = 0, hi = 0; // guest range [base, base+size)
    std::string loud;
};

Bound BindGuest(Gpu& g, const std::array<u32, 4>& d, u64 required) {
    Bound b;
    const u64 base = d[0] | (u64(d[1] & 0xFF) << 32);
    const u32 stride = (d[1] >> 16) & 0x3FFF;
    const u32 num_records = d[2];
    const u32 size = stride == 0 ? num_records : stride * num_records; // GetSize(), u32
    const bool swizzle = (d[1] >> 31) & 1;
    const u32 index_stride = 8u << ((d[3] >> 21) & 3);
    (void)required, (void)swizzle, (void)index_stride;
    // The Apple guards of the BindBuffers under test (build.sh detects each one in its source).
    if (base && num_records) {
#ifdef BB_MIRROR_REQUIRED_SPAN
        if (size < required) {
            b.loud = "binder: macOS structured access escapes bound guest buffer: size=" + std::to_string(size) +
                     ", required_bytes=" + std::to_string(required);
            return b;
        }
#endif
#ifdef BB_MIRROR_SWIZZLE_GROUP
        if (swizzle && num_records % index_stride != 0) {
            b.loud = "binder: macOS incomplete swizzled buffer index group: records=" + std::to_string(num_records) +
                     ", index_stride=" + std::to_string(index_stride);
            return b;
        }
#endif
#ifdef BB_MIRROR_U32_RANGE
        if (stride && u64(stride) * num_records > 0xFFFFFFFFull) {
            b.loud = "binder: macOS structured buffer exceeds 32-bit descriptor range: stride=" + std::to_string(stride) +
                     ", records=" + std::to_string(num_records);
            return b;
        }
#endif
    }
    if (base == 0 || size == 0) {
        b.is_null = true;
        b.info = {.buffer = *g.null_buffer.buffer, .offset = 0, .range = NullBufferSize};
        return b;
    }
    struct R { u64 va; Mem* m; };
    const R regions[] = {{GuestVa, &g.guest}, {ParamVa, &g.params}, {ResultVa, &g.results}};
    for (const R& r : regions) {
        if (base < r.va || base >= r.va + r.m->size) continue;
        const u64 mapped = std::min<u64>(size, r.va + r.m->size - base); // ClampRangeSize
        if (mapped != size) {
            b.loud = "binder: macOS non-null buffer mapping is incomplete: address=" + Hex(base) +
                     ", size=" + std::to_string(size) + ", mapped=" + std::to_string(mapped);
            return b;
        }
        const u64 offset = base - r.va;
        const u64 aligned = offset / g.align * g.align;
        b.adjust = u32(offset - aligned);
        b.info = {.buffer = *r.m->buffer, .offset = aligned, .range = size + b.adjust};
        b.lo = base;
        b.hi = base + size;
        return b;
    }
    b.loud = "binder: macOS non-null buffer mapping is incomplete: address=" + Hex(base) + " not mapped";
    return b;
}

// ---------------------------------------------------------------------------------------------
// Expected outcomes and classification

struct Outcome {
    std::vector<u32> regs;                   // 64 lanes x 4
    std::map<u64, u8> writes;                // memory delta
    std::vector<std::pair<u64, u64>> touches;
    bool unknown = false;
    u32 readings = 0;                         // bitmask over the 7 ambiguity bits: list below
    std::vector<u32> reading_list;
    bool achievable = false;
};

gcn_vsharp ActualV(const Job& j) {
    gcn_vsharp v = j.v;
    if (j.zero_sharp) return v;
    v.base = SlotBase(j.slot) + BaseInArena + j.base_delta;
    return v;
}

std::vector<u32> WaveExpected(const Job& j) {
    const Template& t = g_templates[j.tmpl];
    const u32 threads = t.Threads();
    std::vector<u32> regs(kLanes * 4, ResultFill);
    for (u32 l = 0; l < threads; ++l) {
        for (u32 i = 0; i < 4; ++i) regs[l * 4 + i] = VgprMarker + i;
    }
    for (u32 l = 0; l < threads; ++l) {
        // GCN wave = 64 consecutive invocations; lane = index & 63
        const u32 w = l / 64, lane = l % 64;
        u64 ballot = 0, exec = 0;
        for (u32 k = 0; k < 64 && w * 64 + k < threads; ++k) {
            if (j.lanes[w * 64 + k].data[0] != 0) ballot |= 1ull << k;
            if (j.lanes[w * 64 + k].active != 0) exec |= 1ull << k;
        }
        const u32 blo = u32(ballot), bhi = u32(ballot >> 32);
        auto value = [&](u32 sel) { return j.lanes[w * 64 + (sel & 63)].vindex; };
        u32* r = &regs[l * 4];
        switch (t.wave) {
        case Wave::Lane:
            r[0] = gcn_mbcnt(~0u, gcn_mbcnt(~0u, 0, lane, 0), lane, 1);
            r[1] = blo; r[2] = bhi; r[3] = gcn_popcount64(ballot);
            break;
        case Wave::MbcntMask:
            r[0] = gcn_mbcnt(bhi, gcn_mbcnt(blo, 0, lane, 0), lane, 1);
            r[1] = blo; r[2] = bhi; r[3] = (ballot >> lane) & 1;
            break;
        case Wave::Readlane: // lane selects are kept in 0..63 by the jobs
            r[0] = value(37); r[1] = value(j.smem_sgpr); r[2] = value(j.sgpr_soff); r[3] = value(0);
            break;
        case Wave::Exec1:
            r[0] = value(gcn_first_lane(exec)); r[1] = gcn_popcount64(exec);
            if (exec >> lane & 1) r[3] = value(lane);
            break;
        case Wave::Exec2:
            r[0] = u32(exec); r[1] = u32(exec >> 32);
            if (exec >> lane & 1) r[2] = gcn_mbcnt(u32(exec >> 32), gcn_mbcnt(u32(exec), 0, lane, 0), lane, 1);
            break;
        case Wave::LoopReadFirstLane: {
            const u32 step = (j.lanes[w * 64].vindex & 0xF000u) | 0x1000u; // first exec lane, as masked
            u32 s20 = 0, n = 0;
            do { s20 += step; ++n; } while (s20 < 0x3000u);
            r[0] = n * gcn_popcount64(ballot);
            r[1] = s20;
            break;
        }
        case Wave::Lds: {
            const Lane& L = j.lanes[l];
            r[0] = t.lds_bits == 16 ? (L.vindex & 0xFFFF) : L.vindex;
            if (t.lds_bits == 64) r[1] = L.voffset;
            r[2] = blo;
            r[3] = bhi;
            break;
        }
        case Wave::None:
            break;
        }
    }
    return regs;
}

std::vector<Outcome> Expected(Gpu& g, const Job& j, std::vector<u8>& work, const Bound& tb) {
    const Template& t = g_templates[j.tmpl];
    if (t.wave != Wave::None) {
        Outcome o;
        o.regs = WaveExpected(j);
        o.achievable = true;
        o.reading_list.push_back(0);
        return {o};
    }
    const gcn_vsharp v = ActualV(j);
    std::map<std::string, size_t> index;
    std::vector<Outcome> outs;
    for (u32 r = 0; r < GCN_NUM_READINGS; ++r) {
        Outcome o;
        o.regs.assign(kLanes * 4, 0);
        for (u32 lane = 0; lane < kLanes; ++lane) {
            for (u32 i = 0; i < 4; ++i) o.regs[lane * 4 + i] = t.vertex ? ResultFill : VgprMarker + i;
            if (!t.vertex && lane >= t.Threads()) {
                for (u32 i = 0; i < 4; ++i) o.regs[lane * 4 + i] = ResultFill; // no such invocation
                continue;
            }
            const Lane& L = j.lanes[lane];
            const bool smem = t.smem != SmemSrc::None;
            if (!L.active && !smem) continue;
            gcn_mem mem{GuestVa, work.size(), work.data()};
            gcn_addr a{.idxen = t.idxen, .offen = t.offen, .vgpr_index = L.vindex, .vgpr_offset = L.voffset,
                       .inst_offset = t.inst_offset, .sgpr_offset = Soff(t, j), .lane = lane};
            gcn_op op = t.op;
            if (smem) op.smem_offset = t.smem == SmemSrc::Imm ? t.smem_imm * 4 : j.smem_sgpr;
            gcn_result res;
            if (t.combo) {
                gcn_op sop{};
                sop.kind = GCN_SLOAD;
                sop.ncomp = 1;
                sop.smem_offset = t.smem_imm * 4;
                gcn_result sres;
                gcn_execute(&v, &a, &sop, L.data, r, &mem, &sres);
                o.regs[lane * 4 + 1] = sres.written[0] ? sres.value[0] : SgprMarker;
                if (sres.unknown) o.unknown = true;
                if (sres.touch_lo != sres.touch_hi) o.touches.push_back({sres.touch_lo, sres.touch_hi});
            }
            gcn_execute(&v, &a, &op, L.data, r, &mem, &res);
            if (res.unknown) o.unknown = true;
            if (res.touch_lo != res.touch_hi) o.touches.push_back({res.touch_lo, res.touch_hi});
            if (smem) {
                for (u32 i = 0; i < 4; ++i) {
                    const u32 k = t.smem_pick[i];
                    o.regs[lane * 4 + i] = (k < op.ncomp && res.written[k]) ? res.value[k] : SgprMarker + k;
                }
            } else if (op.kind == GCN_LOAD) {
                for (u32 i = 0; i < op.ncomp; ++i) o.regs[lane * 4 + i] = res.value[i];
            } else if (op.kind == GCN_ATOMIC && op.atomic_return) {
                o.regs[lane * 4] = res.value[0];
                if (op.comp_bytes == 8) o.regs[lane * 4 + 1] = res.value[1];
            }
            // collect and undo memory changes
            if (res.touch_lo != res.touch_hi && (op.kind == GCN_STORE || op.kind == GCN_ATOMIC)) {
                for (u64 va = res.touch_lo; va < res.touch_hi; ++va) {
                    if (va < GuestVa || va >= GuestVa + work.size()) continue;
                    const u64 k = va - GuestVa;
                    if (work[k] != g.pristine[k]) {
                        o.writes[va] = work[k];
                        work[k] = g.pristine[k];
                    }
                }
            }
        }
        if (t.srt) {
            for (u32 lane = 0; lane < t.Threads(); ++lane) o.regs[lane * 4 + 1] = j.vtable[4];
        }
        if (t.dead) { // the load's destination is never stored: only the markers come back
            for (u32 lane = 0; lane < t.Threads(); ++lane) {
                for (u32 i = 0; i < 4; ++i) o.regs[lane * 4 + i] = VgprMarker + i;
            }
        }
        // achievable inside the bound Vulkan range?
        o.achievable = !o.unknown;
        for (auto [lo, hi] : o.touches) {
            if (tb.is_null || lo < tb.lo || hi > tb.hi) o.achievable = false;
        }
        std::string key(reinterpret_cast<const char*>(o.regs.data()), o.regs.size() * 4);
        for (auto [va, byte] : o.writes) {
            key += Hex(va) + ":" + std::to_string(byte) + ",";
        }
        key += o.unknown ? "U" : "K";
        auto [it, fresh] = index.try_emplace(key, outs.size());
        if (fresh) outs.push_back(std::move(o));
        outs[it->second].reading_list.push_back(r);
    }
    return outs;
}

const char* ReadingName(u32 bit) {
    switch (bit) {
    case GCN_R_WRAP32: return "WRAP32";
    case GCN_R_SOFF_SUM: return "SOFF_SUM";
    case GCN_R_END_WHOLE: return "END_WHOLE";
    case GCN_R_SMEM_GE: return "SMEM_GE";
    case GCN_R_SMEM_ZERO: return "SMEM_ZERO";
    case GCN_R_FMT_SEL: return "FMT_SEL";
    case GCN_R_MTBUF_BOUND: return "MTBUF_BOUND";
    case GCN_R_SHORT_ALIGN: return "SHORT_ALIGN";
    case GCN_R_SEL1_FLOAT: return "SEL1_FLOAT";
    }
    return "?";
}

// Readings that distinguish this outcome: bits set in every member reading.
std::string DescribeReadings(const std::vector<u32>& rs) {
    if (std::find(rs.begin(), rs.end(), 0u) != rs.end()) return "literal";
    u32 all = ~0u;
    for (u32 r : rs) all &= r;
    std::string s;
    for (u32 bit = 1; bit < GCN_NUM_READINGS; bit <<= 1) {
        if (all & bit) s += std::string(s.empty() ? "" : "+") + ReadingName(bit);
    }
    return s.empty() ? "alternative" : s;
}

enum class Verdict { Strict, Ambiguous, Fail, KnownMoltenVk, InvalidSpirv, LoudRequired, LoudApproved, LoudUnneeded, Crash, HarnessError };
const char* VerdictName(Verdict v) {
    switch (v) {
    case Verdict::Strict: return "PASS(strict)";
    case Verdict::Ambiguous: return "PASS(ambiguous)";
    case Verdict::Fail: return "FAIL";
    case Verdict::KnownMoltenVk: return "KNOWN-MOLTENVK";
    case Verdict::InvalidSpirv: return "INVALID-SPIRV";
    case Verdict::LoudRequired: return "LOUD(required)";
    case Verdict::LoudApproved: return "LOUD(approved)";
    case Verdict::LoudUnneeded: return "LOUD(not required)";
    case Verdict::Crash: return "CRASH";
    case Verdict::HarnessError: return "HARNESS-ERROR";
    }
    return "?";
}

struct JobResult {
    Verdict verdict = Verdict::HarnessError;
    std::string detail;
};

// ---------------------------------------------------------------------------------------------

struct Options {
    bool rba = true;
    std::string label = "run";
    std::string only;
    u32 batch = MaxBatch;
    u32 max_detail = 60;
    std::string outdir;
    std::string dump_spv;
    std::string compile_only, stage = "cs";
    bool bench = false;
    std::array<u32, 16> ud{};
};

void FillPristine(Gpu& g, const std::vector<u32>& jobs) {
    g.pristine.assign(g.guest.size, 0);
    for (u64 k = 0; k < g.guest.size; ++k) g.pristine[k] = CanaryByte(GuestVa + k);
    for (u32 ji : jobs) {
        const Job& j = g_jobs[ji];
        if (j.zero_sharp) continue;
        const gcn_vsharp v = ActualV(j);
        const u64 lo = v.base, hi = std::min<u64>(v.base + BoundSize(v), SlotBase(j.slot) + Arena);
        for (u64 va = lo; va < hi; ++va) g.pristine[va - GuestVa] = BufferByte(va);
        if (!j.init_words.empty()) std::memcpy(&g.pristine[lo - GuestVa], j.init_words.data(), j.init_words.size() * 4);
    }
}

std::string DescribeValue(const Gpu& g, const Job& j, u32 value) {
    // Where in the job's arena does this 32-bit value occur (helps spot canary reads)?
    const gcn_vsharp v = ActualV(j);
    const u64 lo = SlotBase(j.slot) - GuestVa, hi = lo + Arena;
    std::string s;
    int found = 0;
    for (u64 k = lo; k + 4 <= hi && found < 2; ++k) {
        u32 x;
        std::memcpy(&x, &g.pristine[k], 4);
        if (x == value) {
            const int64_t rel = int64_t(GuestVa + k) - int64_t(v.base);
            const bool inside = rel >= 0 && u64(rel) < BoundSize(v);
            s += std::string(found ? ", " : " = ") + (inside ? "buffer" : "CANARY") + " bytes at base" +
                 (rel >= 0 ? "+" : "-") + Hex(u64(rel >= 0 ? rel : -rel));
            ++found;
        }
    }
    return s;
}

JobResult Evaluate(Gpu& g, const Job& j, const Bound& tb, const u8* guest_after, const u32* results,
                   std::vector<u8>& work, bool null_dirty) {
    const Template& t = g_templates[j.tmpl];
    auto outs = Expected(g, j, work, tb);
    // observed
    std::vector<u32> regs(results, results + kLanes * 4);
    // compare region: arena + every outcome's touch ranges
    std::vector<std::pair<u64, u64>> region{{SlotBase(j.slot), SlotBase(j.slot) + Arena}};
    for (const auto& o : outs) for (auto r : o.touches) region.push_back(r);
    auto mem_matches = [&](const Outcome& o, std::string* diff) {
        int shown = 0;
        bool ok = true;
        for (auto [lo, hi] : region) {
            lo = std::max(lo, GuestVa);
            hi = std::min(hi, GuestVa + g.guest.size);
            for (u64 va = lo; va < hi; ++va) {
                const u64 k = va - GuestVa;
                auto it = o.writes.find(va);
                const u8 want = it != o.writes.end() ? it->second : g.pristine[k];
                if (guest_after[k] != want) {
                    ok = false;
                    if (diff && shown++ < 12) {
                        const int64_t rel = int64_t(va) - int64_t(ActualV(j).base);
                        *diff += "    mem base" + std::string(rel >= 0 ? "+" : "-") +
                                 Hex(u64(rel >= 0 ? rel : -rel)) + ": got " + Hex(guest_after[k]) +
                                 " want " + Hex(want) + (want == g.pristine[k] ? " (unchanged)" : " (written)") +
                                 "\n";
                    }
                    if (!diff) return false;
                }
            }
        }
        return ok;
    };
    std::vector<size_t> matches;
    for (size_t i = 0; i < outs.size(); ++i) {
        if (outs[i].unknown) continue;
        if (outs[i].regs == regs && mem_matches(outs[i], nullptr)) matches.push_back(i);
    }
    JobResult r;
    if (null_dirty) {
        r.verdict = Verdict::Fail;
        r.detail = "  shared null buffer was written during this batch\n";
        return r;
    }
    if (!matches.empty()) {
        const auto& o = outs[matches[0]];
        if (outs.size() == 1) {
            r.verdict = Verdict::Strict;
        } else {
            r.verdict = Verdict::Ambiguous;
            r.detail = "  matched reading: " + DescribeReadings(o.reading_list) + " (" + std::to_string(outs.size()) +
                       " distinct outcomes across readings)\n";
        }
        return r;
    }
    r.verdict = Verdict::Fail;
    // detail against the closest knowable outcome (fewest differing lanes), else the literal one
    const Outcome* lit = nullptr;
    size_t best = ~size_t(0);
    for (const auto& o : outs) {
        if (o.unknown) continue;
        size_t bad = 0;
        for (u32 lane = 0; lane < kLanes; ++lane) {
            for (u32 i = 0; i < 4; ++i) {
                if (regs[lane * 4 + i] != o.regs[lane * 4 + i]) { ++bad; break; }
            }
        }
        if (!mem_matches(o, nullptr)) bad += kLanes;
        if (bad < best) { best = bad; lit = &o; }
    }
    if (!lit) {
        for (const auto& o : outs) {
            if (std::find(o.reading_list.begin(), o.reading_list.end(), 0u) != o.reading_list.end()) lit = &o;
        }
    }
    std::string d;
    if (lit && outs.size() > 1 && !lit->unknown) {
        d += "  closest reading: " + DescribeReadings(lit->reading_list) + "\n";
    }
    if (outs.size() > 1) {
        d += "  " + std::to_string(outs.size()) + " outcomes across readings; none matched\n";
    }
    if (lit && lit->unknown) {
        d += "  literal reading touches guest memory outside the harness image (legal GCN access far past the "
             "bound range): only a loud error can be correct\n";
    }
    if (lit) {
        int shown = 0;
        for (u32 lane = 0; lane < kLanes && shown < 10; ++lane) {
            bool bad = false;
            for (u32 i = 0; i < 4; ++i) bad |= regs[lane * 4 + i] != lit->regs[lane * 4 + i];
            if (!bad) continue;
            ++shown;
            const Lane& L = j.lanes[lane];
            char buf[256];
            std::snprintf(buf, sizeof(buf), "    lane %2u vindex=%#x voffset=%#x:", lane, L.vindex, L.voffset);
            d += buf;
            for (u32 i = 0; i < 4; ++i) {
                const u32 got = regs[lane * 4 + i], want = lit->regs[lane * 4 + i];
                if (got == want) continue;
                std::snprintf(buf, sizeof(buf), " [%u] got %#010x want %#010x%s", i, got, want,
                              DescribeValue(g, j, got).c_str());
                d += buf;
            }
            d += "\n";
        }
        std::string md;
        mem_matches(*lit, &md);
        d += md;
    }
    (void)t;
    r.detail = d;
    return r;
}

// Records the indexed draw of a vertex job, binding vertex buffers the way
// Rasterizer::ResolveVertexBuffers/EmitVertexBuffers do (fixed-function) or none (pulling).
bool RecordDraw(Gpu& g, vk::CommandBuffer cmd, Job& j, const std::vector<vk::DescriptorBufferInfo>& infos,
                const Shader::PushData& push, std::map<u32, Bound>& tbound) {
    const u32 ji = u32(&j - g_jobs.data());
    const bool pulling = VertexPulling();
    Pipe& pipe = GetGfxPipe(g, j, u32(j.binds.size()), pulling);
    if (pipe.result != vk::Result::eSuccess) {
        j.status = Status::LoudPipeline;
        j.message = "graphics pipeline creation failed: " + vk::to_string(pipe.result);
        return false;
    }
    std::vector<vk::Buffer> bufs;
    std::vector<vk::DeviceSize> offsets, sizes, strides;
    std::vector<vk::VertexInputBindingDescription2EXT> bind2;
    std::vector<vk::VertexInputAttributeDescription2EXT> attr2;
    if (!pulling) {
        for (const auto& a : j.vattrs) {
            const u64 base = a.sharp[0] | (u64(a.sharp[1] & 0xFF) << 32);
            const u32 stride = (a.sharp[1] >> 16) & 0x3FFF;
            const u32 size = stride == 0 ? a.sharp[2] : stride * a.sharp[2];
            if (base != 0 && size > 0) {
                if (base < GuestVa || base + size > GuestVa + g.guest.size) {
                    j.status = Status::HarnessError;
                    j.message = "vertex buffer outside the harness guest image";
                    return false;
                }
                bufs.push_back(*g.guest.buffer);
                offsets.push_back(base - GuestVa);
                sizes.push_back(size);
                if (!tbound.count(ji)) tbound[ji] = Bound{.lo = base, .hi = base + size};
            } else {
                bufs.push_back(*g.null_buffer.buffer);
                offsets.push_back(0);
                sizes.push_back(0);
                if (!tbound.count(ji)) tbound[ji] = Bound{.is_null = true};
            }
            strides.push_back(stride);
            bind2.push_back({.binding = a.semantic, .stride = stride, .inputRate = vk::VertexInputRate::eVertex,
                             .divisor = 1});
            attr2.push_back({.location = a.semantic, .binding = a.semantic, .format = vk::Format(a.format), .offset = 0});
        }
    }
    u32* idx = reinterpret_cast<u32*>(g.index.data + u64(j.slot) * 256);
    u32 count = 0;
    for (u32 lane = 0; lane < 64; ++lane) {
        if (j.lanes[lane].active) idx[count++] = j.lanes[lane].vindex - u32(j.vertex_offset);
    }
    cmd.beginRendering({.renderArea = {.extent = {1, 1}}, .layerCount = 1});
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *pipe.pipeline);
    BindStorageBuffers(g, cmd, vk::PipelineBindPoint::eGraphics, pipe, infos);
    cmd.pushConstants(*pipe.playout, vk::ShaderStageFlagBits::eVertex, 0, sizeof(push), &push);
    if (!pulling && !bufs.empty()) {
        if (g.dynamic_vertex_input) {
            cmd.setVertexInputEXT(bind2, attr2);
            cmd.bindVertexBuffers(0, u32(bufs.size()), bufs.data(), offsets.data());
        } else {
            cmd.bindVertexBuffers2(0, u32(bufs.size()), bufs.data(), offsets.data(), sizes.data(), strides.data());
        }
    }
    cmd.bindIndexBuffer(*g.index.buffer, u64(j.slot) * 256, vk::IndexType::eUint32);
    if (g_templates[j.tmpl].indirect) {
        const vk::DrawIndexedIndirectCommand args{.indexCount = count, .instanceCount = 1, .firstIndex = 0,
                                                  .vertexOffset = j.vertex_offset, .firstInstance = 0};
        std::memcpy(g.indirect.data + u64(j.slot) * 32, &args, sizeof(args));
        cmd.drawIndexedIndirect(*g.indirect.buffer, u64(j.slot) * 32, 1, sizeof(args));
    } else {
        cmd.drawIndexed(count, 1, 0, j.vertex_offset, 0);
    }
    cmd.endRendering();
    return true;
}

struct RenderTarget {
    vk::UniqueImage image;
    vk::UniqueDeviceMemory memory;
    vk::UniqueImageView view;
};

// VS (+ the RectList aux TCS/TES the GraphicsPipeline adds) + FS into a 16x16 RGBA32F target.
// Vertex input mirrors GetVertexInputs when the tree does not pull vertices in the shader.
bool RecordRender(Gpu& g, vk::CommandBuffer cmd, Job& j, const std::vector<vk::DescriptorBufferInfo>& infos,
                  const Shader::PushData& push, std::vector<RenderTarget>& targets) {
    const Template& t = g_templates[j.tmpl];
    const bool pulling = VertexPulling();
    static std::map<std::string, Pipe> pipes;
    const std::string key = std::to_string(j.tmpl) + "/" + std::to_string(j.perm);
    auto [it, fresh] = pipes.try_emplace(key);
    Pipe& p = it->second;
    static std::map<std::string, std::vector<vk::UniqueShaderModule>> module_store; // live as long as the pipelines
    std::vector<vk::UniqueShaderModule>& modules = module_store[key];
    if (fresh) {
        auto module = [&](const std::vector<u32>& spv) {
            modules.push_back(g.device->createShaderModuleUnique({.codeSize = spv.size() * 4, .pCode = spv.data()}).value);
            return *modules.back();
        };
        // The render FS reads no inputs. BB_RENDER_FS_INPUTS=N declares N anyway (VS exports none):
        // on MoltenVK the RectList aux TCS then passes unwritten locations and nothing is drawn.
        const auto fs_rt = MakeRuntime(false, true, false, {64, 1, 1}, false, 0, false, RenderFsInputs());
        std::vector<vk::PipelineShaderStageCreateInfo> stages;
        stages.push_back({.stage = vk::ShaderStageFlagBits::eVertex, .module = module(g_spirv.at({j.tmpl, j.perm})), .pName = "main"});
        if (t.rect_list) {
            using Shader::Backend::SPIRV::AuxShaderType;
            stages.push_back({.stage = vk::ShaderStageFlagBits::eTessellationControl,
                              .module = module(AuxTess(AuxShaderType::RectListTCS, fs_rt.hw.fs)),
                              .pName = "main"});
            stages.push_back({.stage = vk::ShaderStageFlagBits::eTessellationEvaluation,
                              .module = module(AuxTess(AuxShaderType::PassthroughTES, fs_rt.hw.fs)),
                              .pName = "main"});
        }
        stages.push_back({.stage = vk::ShaderStageFlagBits::eFragment, .module = module(g_spirv.at({t.fs_tmpl, 0})), .pName = "main"});
        if (const char* dir = std::getenv("BB_DUMP_RENDER")) { // debugging aid: the exact stage SPIR-V
            auto dump = [&](const std::string& n, const std::vector<u32>& spv) {
                if (FILE* f = std::fopen((std::string(dir) + "/" + key.substr(0, key.find('/')) + "-" + n + ".spv").c_str(), "wb")) {
                    std::fwrite(spv.data(), 4, spv.size(), f);
                    std::fclose(f);
                }
            };
            using Shader::Backend::SPIRV::AuxShaderType;
            dump("vs", g_spirv.at({j.tmpl, j.perm}));
            dump("fs", g_spirv.at({t.fs_tmpl, 0}));
            if (t.rect_list) {
                dump("tcs", AuxTess(AuxShaderType::RectListTCS, fs_rt.hw.fs));
                dump("tes", AuxTess(AuxShaderType::PassthroughTES, fs_rt.hw.fs));
            }
        }
        const auto all = vk::ShaderStageFlagBits::eAllGraphics;
        std::vector<vk::DescriptorSetLayoutBinding> b;
        for (u32 i = 0; i < j.binds.size(); ++i) {
            b.push_back({.binding = i, .descriptorType = vk::DescriptorType::eStorageBuffer, .descriptorCount = 1, .stageFlags = all});
        }
        p.push = UsePushDescriptors(u32(b.size()));
        p.layout = g.device->createDescriptorSetLayoutUnique({.flags = p.push ? vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR : vk::DescriptorSetLayoutCreateFlags{}, .bindingCount = u32(b.size()), .pBindings = b.data()}).value;
        const vk::PushConstantRange pr{.stageFlags = all, .offset = 0, .size = sizeof(Shader::PushData)};
        const vk::DescriptorSetLayout l = *p.layout;
        p.playout = g.device->createPipelineLayoutUnique({.setLayoutCount = 1, .pSetLayouts = &l,
                                                          .pushConstantRangeCount = 1, .pPushConstantRanges = &pr}).value;
        std::vector<vk::VertexInputBindingDescription> vb;
        std::vector<vk::VertexInputAttributeDescription> va;
        std::vector<vk::DynamicState> dyn;
        if (!pulling && !j.vattrs.empty()) {
            if (g.dynamic_vertex_input) {
                dyn.push_back(vk::DynamicState::eVertexInputEXT);
            } else {
                for (const auto& a : j.vattrs) {
                    vb.push_back({.binding = a.semantic, .stride = (a.sharp[1] >> 16) & 0x3FFF, .inputRate = vk::VertexInputRate::eVertex});
                    va.push_back({.location = a.semantic, .binding = a.semantic, .format = vk::Format(a.format), .offset = 0});
                }
                dyn.push_back(vk::DynamicState::eVertexInputBindingStride);
            }
        }
        const vk::PipelineVertexInputStateCreateInfo vi{.vertexBindingDescriptionCount = u32(vb.size()), .pVertexBindingDescriptions = vb.data(),
                                                        .vertexAttributeDescriptionCount = u32(va.size()), .pVertexAttributeDescriptions = va.data()};
        const vk::PipelineInputAssemblyStateCreateInfo ia{.topology = t.rect_list ? vk::PrimitiveTopology::ePatchList : vk::PrimitiveTopology::eTriangleList};
        const vk::PipelineTessellationStateCreateInfo ts{.patchControlPoints = 3};
        const vk::Viewport vp{.x = 0, .y = 0, .width = float(RenderSize), .height = float(RenderSize), .minDepth = 0, .maxDepth = 1};
        const vk::Rect2D sc{.offset = {0, 0}, .extent = {RenderSize, RenderSize}};
        const vk::PipelineViewportStateCreateInfo vps{.viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sc};
        const vk::PipelineRasterizationStateCreateInfo rs{.polygonMode = vk::PolygonMode::eFill, .cullMode = vk::CullModeFlagBits::eNone,
                                                          .frontFace = vk::FrontFace::eCounterClockwise, .lineWidth = 1.0f};
        const vk::PipelineMultisampleStateCreateInfo ms{.rasterizationSamples = vk::SampleCountFlagBits::e1};
        const vk::PipelineColorBlendAttachmentState cba{.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                                                                          vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA};
        const vk::PipelineColorBlendStateCreateInfo cb{.attachmentCount = 1, .pAttachments = &cba};
        const vk::PipelineDynamicStateCreateInfo ds{.dynamicStateCount = u32(dyn.size()), .pDynamicStates = dyn.data()};
        const vk::Format fmt = vk::Format::eR32G32B32A32Sfloat;
        const vk::PipelineRenderingCreateInfo rendering{.colorAttachmentCount = 1, .pColorAttachmentFormats = &fmt};
        auto pipe = g.device->createGraphicsPipelineUnique(
            {}, {.pNext = &rendering, .stageCount = u32(stages.size()), .pStages = stages.data(), .pVertexInputState = &vi,
                 .pInputAssemblyState = &ia, .pTessellationState = t.rect_list ? &ts : nullptr, .pViewportState = &vps,
                 .pRasterizationState = &rs, .pMultisampleState = &ms, .pColorBlendState = &cb, .pDynamicState = &ds,
                 .layout = *p.playout});
        p.result = pipe.result;
        if (pipe.result == vk::Result::eSuccess) p.pipeline = std::move(pipe.value);
    }
    if (p.result != vk::Result::eSuccess) {
        j.status = Status::LoudPipeline;
        j.message = "render pipeline creation failed: " + vk::to_string(p.result);
        return false;
    }
    // target
    RenderTarget rt;
    rt.image = g.device->createImageUnique({.imageType = vk::ImageType::e2D, .format = vk::Format::eR32G32B32A32Sfloat,
                                           .extent = {RenderSize, RenderSize, 1}, .mipLevels = 1, .arrayLayers = 1,
                                           .samples = vk::SampleCountFlagBits::e1, .tiling = vk::ImageTiling::eOptimal,
                                           .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferSrc}).value;
    const auto req = g.device->getImageMemoryRequirements(*rt.image);
    const auto props = g.physical.getMemoryProperties();
    u32 type = 0;
    while (!(req.memoryTypeBits & (1u << type))) ++type;
    rt.memory = g.device->allocateMemoryUnique({.allocationSize = req.size, .memoryTypeIndex = type}).value;
    (void)props;
    (void)g.device->bindImageMemory(*rt.image, *rt.memory, 0);
    const vk::ImageSubresourceRange range{.aspectMask = vk::ImageAspectFlagBits::eColor, .levelCount = 1, .layerCount = 1};
    rt.view = g.device->createImageViewUnique({.image = *rt.image, .viewType = vk::ImageViewType::e2D,
                                               .format = vk::Format::eR32G32B32A32Sfloat, .subresourceRange = range}).value;
    const vk::ImageMemoryBarrier to_color{.dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                                          .oldLayout = vk::ImageLayout::eUndefined, .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
                                          .image = *rt.image, .subresourceRange = range};
    cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe, vk::PipelineStageFlagBits::eColorAttachmentOutput, {}, {}, {}, to_color);
    const vk::RenderingAttachmentInfo att{.imageView = *rt.view, .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
                                          .loadOp = vk::AttachmentLoadOp::eClear, .storeOp = vk::AttachmentStoreOp::eStore,
                                          .clearValue = vk::ClearValue{.color = vk::ClearColorValue{std::array<float, 4>{ClearColor[0], ClearColor[1], ClearColor[2], ClearColor[3]}}}};
    cmd.beginRendering({.renderArea = {.extent = {RenderSize, RenderSize}}, .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &att});
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *p.pipeline);
    if (!infos.empty()) {
        BindStorageBuffers(g, cmd, vk::PipelineBindPoint::eGraphics, p, infos);
    }
    cmd.pushConstants(*p.playout, vk::ShaderStageFlagBits::eAllGraphics, 0, sizeof(push), &push);
    if (!pulling && !j.vattrs.empty()) {
        std::vector<vk::Buffer> bufs;
        std::vector<vk::DeviceSize> offs, sizes, strides;
        std::vector<vk::VertexInputBindingDescription2EXT> bind2;
        std::vector<vk::VertexInputAttributeDescription2EXT> attr2;
        for (const auto& a : j.vattrs) {
            const u64 base = a.sharp[0] | (u64(a.sharp[1] & 0xFF) << 32);
            const u32 stride = (a.sharp[1] >> 16) & 0x3FFF;
            bufs.push_back(*g.guest.buffer);
            offs.push_back(base - GuestVa);
            sizes.push_back(u64(stride) * a.sharp[2]);
            strides.push_back(stride);
            bind2.push_back({.binding = a.semantic, .stride = stride, .inputRate = vk::VertexInputRate::eVertex, .divisor = 1});
            attr2.push_back({.location = a.semantic, .binding = a.semantic, .format = vk::Format(a.format), .offset = 0});
        }
        if (g.dynamic_vertex_input) {
            cmd.setVertexInputEXT(bind2, attr2);
            cmd.bindVertexBuffers(0, u32(bufs.size()), bufs.data(), offs.data());
        } else {
            cmd.bindVertexBuffers2(0, u32(bufs.size()), bufs.data(), offs.data(), sizes.data(), strides.data());
        }
    }
    cmd.draw(3, 1, 0, 0);
    cmd.endRendering();
    const vk::ImageMemoryBarrier to_src{.srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite, .dstAccessMask = vk::AccessFlagBits::eTransferRead,
                                        .oldLayout = vk::ImageLayout::eColorAttachmentOptimal, .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                                        .image = *rt.image, .subresourceRange = range};
    cmd.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput, vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, to_src);
    const vk::BufferImageCopy copy{.bufferOffset = u64(j.slot) * RenderBytes,
                                   .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor, .layerCount = 1},
                                   .imageExtent = {RenderSize, RenderSize, 1}};
    cmd.copyImageToBuffer(*rt.image, vk::ImageLayout::eTransferSrcOptimal, *g.readback.buffer, copy);
    targets.push_back(std::move(rt));
    return true;
}

JobResult EvaluateRender(Gpu& g, const Job& j) {
    const float* px = reinterpret_cast<const float*>(g.readback.data + u64(j.slot) * RenderBytes);
    u32 bad = 0;
    std::string d;
    for (u32 y = 0; y < RenderSize; ++y) {
        for (u32 x = 0; x < RenderSize; ++x) {
            const bool covered = !j.quarter || (x < RenderSize / 2 && y < RenderSize / 2);
            const float* want = covered ? RenderColor : ClearColor;
            const float* got = px + (y * RenderSize + x) * 4;
            if (std::memcmp(got, want, 16) != 0) {
                if (bad++ < 4) {
                    char b[160];
                    std::snprintf(b, sizeof(b), "    pixel (%u,%u): got (%g %g %g %g) want (%g %g %g %g)\n", x, y, got[0], got[1],
                                  got[2], got[3], want[0], want[1], want[2], want[3]);
                    d += b;
                }
            }
        }
    }
    if (!bad) return {Verdict::Strict, ""};
    return {Verdict::Fail, "  " + std::to_string(bad) + " of " + std::to_string(RenderSize * RenderSize) +
                               " pixels wrong (" + (j.quarter ? "quarter" : "full") + " coverage expected)\n" + d};
}

// Run one batch of jobs; returns per-job results (jobs that cannot run get loud/error verdicts).
std::map<u32, JobResult> RunBatch(Gpu& g, const std::vector<u32>& jobs) {
    std::map<u32, JobResult> out;
    FillPristine(g, jobs);
    std::memcpy(g.guest.data, g.pristine.data(), g.guest.size);
    std::fill_n(reinterpret_cast<u32*>(g.results.data), g.results.size / 4, ResultFill);
    std::memset(g.null_buffer.data, 0, NullBufferSize);
    assert(g.device->resetDescriptorPool(*g.dpool) == vk::Result::eSuccess);
    auto cmd = g.device->allocateCommandBuffers({.commandPool = *g.pool, .level = vk::CommandBufferLevel::ePrimary,
                                                 .commandBufferCount = 1}).value.front();
    assert(cmd.begin(vk::CommandBufferBeginInfo{.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit}) ==
           vk::Result::eSuccess);
    std::map<u32, Bound> tbound;
    std::vector<u32> ran;
    std::vector<RenderTarget> targets;
    for (u32 ji : jobs) {
        Job& j = g_jobs[ji];
        // params
        u32* p = reinterpret_cast<u32*>(g.params.data + u64(j.slot) * ParamSlot);
        for (u32 lane = 0; lane < kLanes; ++lane) {
            const Lane& L = j.lanes[lane];
            const u32 w[8] = {L.active, L.vindex, L.voffset, L.data[0], L.data[1], L.data[2], L.data[3], 0};
            std::memcpy(p + lane * 8, w, 32);
        }
        if (j.status != Status::Ok || g_templates[j.tmpl].compile_only || g_templates[j.tmpl].helper) continue;
        // bind
        std::vector<vk::DescriptorBufferInfo> infos;
        Shader::PushData push{};
        std::copy(j.ud_regs.begin(), j.ud_regs.end(), push.ud_regs.begin());
        std::string loud;
        u32 flat_used = 0;
        for (u32 b = 0; b < j.binds.size(); ++b) {
            const BindPlan& bp = j.binds[b];
            if (bp.type == u32(Shader::BufferType::Guest)) {
                Bound bd = BindGuest(g, bp.sharp, bp.required);
                if (!bd.loud.empty()) {
                    loud = bd.loud;
                    break;
                }
                push.AddOffset(b, bd.adjust);
                infos.push_back(bd.info);
                // the buffer under test: user data s[0:3], or the fetched V# for vertex jobs
                const auto& tt = g_templates[j.tmpl];
                const std::array<u32, 4> under_test =
                    (tt.vertex || tt.srt) ? std::array<u32, 4>{j.vtable[0], j.vtable[1], j.vtable[2], j.vtable[3]}
                                          : std::array<u32, 4>{j.ud[0], j.ud[1], j.ud[2], j.ud[3]};
                if (bp.sharp == under_test && !tbound.count(ji)) {
                    tbound[ji] = bd;
                }
            } else if (bp.type == u32(Shader::BufferType::ClipPlanes)) {
                // BindBuffers: user clip planes are disabled here, so the null resource is bound.
                push.AddOffset(b, 0);
                infos.push_back({.buffer = *g.null_buffer.buffer, .offset = 0, .range = NullBufferSize});
            } else if (bp.type == u32(Shader::BufferType::Flatbuf)) {
                const u64 off = u64(j.slot) * FlatSlot + flat_used;
                const u64 bytes = bp.flat.size() * 4;
                assert(flat_used + bytes <= FlatSlot);
                std::memcpy(g.flat.data + off, bp.flat.data(), bytes);
                infos.push_back({.buffer = *g.flat.buffer, .offset = off, .range = std::max<u64>(bytes, 4)});
                flat_used += u32((bytes + g.align - 1) / g.align * g.align);
            } else {
                out[ji] = {Verdict::HarnessError, "  unsupported buffer type " + std::to_string(bp.type) + "\n"};
                loud = "-";
                break;
            }
        }
        if (out.count(ji)) continue;
        if (!loud.empty()) {
            j.status = Status::LoudBinder;
            j.message = loud;
            continue;
        }
        if (g_templates[j.tmpl].render) {
            if (RecordRender(g, cmd, j, infos, push, targets)) ran.push_back(ji);
            continue;
        }
        if (g_templates[j.tmpl].vertex) {
            if (RecordDraw(g, cmd, j, infos, push, tbound)) ran.push_back(ji);
            continue;
        }
        Pipe& pipe = GetPipe(g, j.tmpl, j.perm, u32(j.binds.size()), j.force_push);
        if (pipe.result != vk::Result::eSuccess) {
            j.status = Status::LoudPipeline;
            j.message = "pipeline creation failed: " + vk::to_string(pipe.result);
            continue;
        }
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *pipe.pipeline);
        BindStorageBuffers(g, cmd, vk::PipelineBindPoint::eCompute, pipe, infos);
        cmd.pushConstants(*pipe.playout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push), &push);
        cmd.dispatch(1, 1, 1);
        ran.push_back(ji);
    }
    const vk::MemoryBarrier barrier{.srcAccessMask = vk::AccessFlagBits::eShaderWrite,
                                    .dstAccessMask = vk::AccessFlagBits::eHostRead};
    cmd.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eHost, {}, barrier, {}, {});
    assert(cmd.end() == vk::Result::eSuccess);
    const vk::SubmitInfo submit{.commandBufferCount = 1, .pCommandBuffers = &cmd};
    assert(g.queue.submit(submit) == vk::Result::eSuccess);
    const auto wait = g.queue.waitIdle();
    if (wait != vk::Result::eSuccess) {
        std::fprintf(stderr, "gcn-conformance: queue wait failed: %s\n", vk::to_string(wait).c_str());
        std::exit(3);
    }
    g.device->freeCommandBuffers(*g.pool, cmd);
    bool null_dirty = false;
    for (u32 i = 0; i < NullBufferSize; ++i) null_dirty |= g.null_buffer.data[i] != 0;
    std::vector<u8> work = g.pristine;
    for (u32 ji : ran) {
        const Job& j = g_jobs[ji];
        Bound tb = tbound.count(ji) ? tbound[ji] : Bound{.is_null = true};
        if (g_templates[j.tmpl].render) {
            out[ji] = EvaluateRender(g, j);
            continue;
        }
        const u32* res = reinterpret_cast<const u32*>(g.results.data + u64(j.slot) * ResultSlot);
        // the null buffer is shared: attribute a dirty null buffer only to jobs with a null V#
        out[ji] = Evaluate(g, j, tb, g.guest.data, res, work, null_dirty && tb.is_null);
    }
    return out;
}

bool LoudRequired(Gpu& g, Job& j) {
    // Required when no reading's outcome stays inside the range the binder would bind.
    std::vector<u8> work = g.pristine;
    FillPristine(g, {u32(&j - g_jobs.data())});
    work = g.pristine;
    Bound tb;
    if (j.zero_sharp) {
        tb.is_null = true;
    } else {
        const gcn_vsharp v = ActualV(j);
        const u64 size = BoundSize(v);
        if (size == 0) tb.is_null = true;
        tb.lo = v.base;
        tb.hi = v.base + size;
    }
    for (const auto& o : Expected(g, j, work, tb)) {
        if (o.achievable) return false;
    }
    return true;
}

} // namespace

// --bench: GPU time of K ballot rounds (VOPC -> VCC read as a value, s_bcnt1, v_mbcnt over the
// mask) per invocation, with and without the Apple wave64 emulation, over many workgroups.
Template BenchTemplate(u32 rounds, std::array<u32, 3> wg, u32 salt) {
    using namespace GcnAsm;
    Template t;
    t.name = "bench rounds=" + std::to_string(rounds);
    t.wg = wg;
    Code c;
    c.SMovLiteral(VCC_HI, 0x5eed);
    u32 log2x = 0;
    while ((1u << log2x) < wg[0]) ++log2x;
    c.VLshlrevB32(15, Inline(log2x), 1);
    c.Vop2(37, 15, 15, Vgpr(0));    // v15 = local invocation index
    c.VMov(10, Inline(0));
    c.VMov(11, Inline(0));
    for (u32 r = 0; r < rounds; ++r) {
        c.Vop2(27, 12, 15, Inline(1 + r % 31)); // v12 = v15 & k
        c.VCmpNeU32(Inline(0), 12);            // vcc = ballot(v12 != 0)
        c.SBcnt1B64(16, VCC_LO);
        c.VMbcnt(false, 13, VCC_LO, 10);
        c.VMbcnt(true, 10, VCC_HI, 13);        // v10 += lanes below with the bit set
        c.Vop2(37, 11, 11, 16);                // v11 += s16 (popcount)
    }
    c.VLshlrevB32(14, Inline(3), 15);
    c.Mubuf(BUFFER_STORE_DWORDX2, Code::BufFields{.offen = true, .vaddr = 14, .vdata = 10, .srsrc = 8});
    c.SWaitcntAll();
    c.SEndpgm();
    t.code = std::move(c.words);
    t.hash = 0xbe9c00ull ^ (u64(rounds) << 8) ^ (u64(wg[0] * wg[1]) << 20) ^ (u64(salt) << 40);
    return t;
}

int Bench(const Options& opt) {
    struct Variant { u32 rounds; std::array<u32, 3> wg; bool emulate; };
    const Variant variants[] = {{0, {64, 1, 1}, true},   {16, {64, 1, 1}, false}, {16, {64, 1, 1}, true},
                                {0, {256, 1, 1}, true},  {16, {256, 1, 1}, false}, {16, {256, 1, 1}, true}};
    const Shader::Profile saved = g_profile;
    std::vector<std::pair<u32, Job>> compiled; // template index, job
    for (u32 i = 0; i < std::size(variants); ++i) {
        const auto& v = variants[i];
        g_profile = saved;
        [&](auto& prof) {
            if constexpr (requires { prof.emulate_wave64; }) prof.emulate_wave64 = v.emulate;
        }(g_profile);
        g_templates.push_back(BenchTemplate(v.rounds, v.wg, i));
        const u32 ti = u32(g_templates.size() - 1);
        Job j;
        j.tmpl = ti;
        j.group = "bench";
        gcn_vsharp ov = RawV(256 * 8);
        ov.base = GuestVa;
        const auto o = Pack(ov);
        for (u32 k = 0; k < 4; ++k) j.ud[8 + k] = o[k];
        g_jobs = {j};
        CompileAll(opt.outdir);
        if (g_jobs[0].status != Status::Ok) {
            std::printf("bench variant %u: compile failed: %s\n", i, g_jobs[0].message.c_str());
            return 1;
        }
        compiled.push_back({ti, g_jobs[0]});
    }
    g_profile = saved;
    Gpu g;
    InitGpu(g, true);
    const auto props = g.physical.getProperties();
    auto qpool = g.device->createQueryPoolUnique({.queryType = vk::QueryType::eTimestamp, .queryCount = 2}).value;
    constexpr u32 kWorkgroups = 16384;
    std::printf("bench: %u workgroups, timestampPeriod %.2f ns\n", kWorkgroups, props.limits.timestampPeriod);
    for (u32 i = 0; i < compiled.size(); ++i) {
        auto& [ti, j] = compiled[i];
        const auto& v = variants[i];
        Pipe& pipe = GetPipe(g, ti, j.perm, u32(j.binds.size()));
        if (pipe.result != vk::Result::eSuccess) return 1;
        std::vector<vk::DescriptorBufferInfo> infos;
        Shader::PushData push{};
        std::copy(j.ud_regs.begin(), j.ud_regs.end(), push.ud_regs.begin());
        for (u32 b = 0; b < j.binds.size(); ++b) {
            if (j.binds[b].type == u32(Shader::BufferType::Guest)) {
                Bound bd = BindGuest(g, j.binds[b].sharp, j.binds[b].required);
                push.AddOffset(b, bd.adjust);
                infos.push_back(bd.info);
            } else {
                infos.push_back({.buffer = *g.flat.buffer, .offset = 0, .range = std::max<u64>(j.binds[b].flat.size() * 4, 4)});
                std::memcpy(g.flat.data, j.binds[b].flat.data(), j.binds[b].flat.size() * 4);
            }
        }
        double best = 1e30;
        for (u32 rep = 0; rep < 5; ++rep) {
            assert(g.device->resetDescriptorPool(*g.dpool) == vk::Result::eSuccess);
            auto cmd = g.device->allocateCommandBuffers({.commandPool = *g.pool, .level = vk::CommandBufferLevel::ePrimary,
                                                         .commandBufferCount = 1}).value.front();
            (void)cmd.begin(vk::CommandBufferBeginInfo{.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
            cmd.resetQueryPool(*qpool, 0, 2);
            cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *pipe.pipeline);
            BindStorageBuffers(g, cmd, vk::PipelineBindPoint::eCompute, pipe, infos);
            cmd.pushConstants(*pipe.playout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push), &push);
            cmd.writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe, *qpool, 0);
            cmd.dispatch(kWorkgroups * 64 / v.wg[0], 1, 1);
            cmd.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, *qpool, 1);
            (void)cmd.end();
            const vk::SubmitInfo submit{.commandBufferCount = 1, .pCommandBuffers = &cmd};
            (void)g.queue.submit(submit);
            (void)g.queue.waitIdle();
            u64 ts[2]{};
            (void)g.device->getQueryPoolResults(*qpool, 0, 2, sizeof(ts), ts, sizeof(u64),
                                                vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWait);
            g.device->freeCommandBuffers(*g.pool, cmd);
            best = std::min(best, double(ts[1] - ts[0]) * props.limits.timestampPeriod);
        }
        const double invocations = double(kWorkgroups) * 64;
        std::printf("  wg %3ux1  rounds %2u  %-22s %8.3f ms  %6.2f ns/invocation%s\n", v.wg[0], v.rounds,
                    v.rounds == 0 ? "(baseline)" : (v.emulate ? "wave64 emulation" : "32-wide subgroups (wrong)"),
                    best / 1e6, best / invocations,
                    v.rounds ? ("  " + std::to_string(best / invocations / v.rounds) + " ns/ballot").c_str() : "");
    }
    return 0;
}

// --disasm FILE: decode a GCN binary with the production decoder and flag wave-level operations
// (results depend on the 64-lane wave; see the wave64 group).
int Disasm(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (!f) { std::perror(path); return 2; }
    std::vector<u32> code;
    u32 w;
    while (std::fread(&w, 4, 1, f) == 1) code.push_back(w);
    std::fclose(f);
    Shader::Gcn::GcnCodeSlice slice(code.data(), code.data() + code.size());
    Shader::Gcn::GcnDecodeContext decoder;
    std::map<std::string, u32> wave_ops;
    u32 n = 0, pc = 0;
    auto reads = [](const Shader::Gcn::GcnInst& in, Shader::Gcn::OperandField fld) {
        for (u32 i = 0; i < in.src_count; ++i) if (in.src[i].field == fld) return true;
        return false;
    };
    while (!slice.atEnd()) {
        const auto inst = decoder.decodeInstruction(slice);
        const std::string name{magic_enum::enum_name(inst.opcode)};
        using OF = Shader::Gcn::OperandField;
        std::string why;
        if (name.find("READLANE") != std::string::npos || name.find("READFIRSTLANE") != std::string::npos ||
            name.find("WRITELANE") != std::string::npos || name.find("MBCNT") != std::string::npos ||
            name.find("SWIZZLE") != std::string::npos || name.find("_B64") != std::string::npos ||
            name.find("SAVEEXEC") != std::string::npos || name.find("BCNT") != std::string::npos ||
            name.find("FF1") != std::string::npos || name.find("FLBIT") != std::string::npos) {
            why = name;
        }
        if (reads(inst, OF::ExecHi) || reads(inst, OF::VccHi)) why += " reads exec_hi/vcc_hi";
        if (name.rfind("V_", 0) == 0 && (reads(inst, OF::VccLo) || reads(inst, OF::ExecLo)) &&
            name.find("CNDMASK") == std::string::npos && name.find("ADDC") == std::string::npos &&
            name.find("SUBB") == std::string::npos) {
            why += " vector reads vcc/exec as a value";
        }
        if (!why.empty()) {
            ++wave_ops[name];
            std::printf("%5x: %s%s\n", pc * 4, name.c_str(), why == name ? "" : (" (" + why + ")").c_str());
        }
        pc += inst.length / 4;
        ++n;
    }
    std::printf("%u instructions; wave-relevant opcode counts:\n", n);
    for (auto& [k, c] : wave_ops) std::printf("  %-28s %u\n", k.c_str(), c);
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--disasm") return Disasm(argv[2]);
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--no-rba") opt.rba = false;
        else if (a == "--label" && i + 1 < argc) opt.label = argv[++i];
        else if (a == "--only" && i + 1 < argc) opt.only = argv[++i];
        else if (a == "--batch" && i + 1 < argc) opt.batch = std::clamp<u32>(std::atoi(argv[++i]), 1, MaxBatch);
        else if (a == "--max-detail" && i + 1 < argc) opt.max_detail = std::atoi(argv[++i]);
        else if (a == "--out" && i + 1 < argc) opt.outdir = argv[++i];
        else if (a == "--dump-spv" && i + 1 < argc) opt.dump_spv = argv[++i];
        else if (a == "--compile-only" && i + 1 < argc) opt.compile_only = argv[++i];
        else if (a == "--bench") opt.bench = true;
        else if (a == "--descriptor-sets") g_force_sets = true;
        else if (a == "--push-descriptors") g_force_sets = false;
        else if (a == "--stage" && i + 1 < argc) opt.stage = argv[++i];
        else if (a == "--fs-export" && i + 1 < argc) g_fs_export = u32(std::atoi(argv[++i]));
        else if (a == "--ud" && i + 1 < argc) {
            std::string list = argv[++i];
            u32 k = 0;
            for (size_t pos = 0; pos <= list.size() && k < 16;) {
                size_t e = list.find(',', pos);
                if (e == std::string::npos) e = list.size();
                opt.ud[k++] = u32(std::strtoul(list.substr(pos, e - pos).c_str(), nullptr, 0));
                pos = e + 1;
            }
        }
        else {
            std::fprintf(stderr, "usage: %s [--no-rba] [--label NAME] [--only SUBSTRING] [--batch N] "
                                 "[--max-detail N] [--dump-spv DIR] --out DIR\n", argv[0]);
            return 2;
        }
    }
    if (opt.outdir.empty()) {
        std::fprintf(stderr, "gcn-conformance: --out DIR is required\n");
        return 2;
    }
    {
        std::error_code ec;
        std::filesystem::create_directories(opt.outdir, ec);
        if (ec) {
            std::fprintf(stderr, "gcn-conformance: cannot create %s: %s\n", opt.outdir.c_str(), ec.message().c_str());
            return 2;
        }
    }
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    // Phase 0: device capabilities in a child process.
    {
        int fds[2];
        if (pipe(fds) != 0) return 2;
        std::fflush(nullptr);
        const pid_t pid = fork();
        if (pid == 0) {
            close(fds[0]);
            const DeviceCaps c = QueryCaps();
            if (write(fds[1], &c, sizeof(c)) != ssize_t(sizeof(c))) _exit(1);
            _exit(0);
        }
        close(fds[1]);
        const ssize_t n = read(fds[0], &g_caps, sizeof(g_caps));
        close(fds[0]);
        int status = 0;
        waitpid(pid, &status, 0);
        if (n != ssize_t(sizeof(g_caps)) || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            std::fprintf(stderr, "gcn-conformance: device query child failed\n");
            return 2;
        }
    }
    g_profile = MakeProfile(g_caps);
    std::printf("device: api %u.%u subgroup %u storage-align %u int8 %u int16 %u int64 %u fp64 %u rba %u rba2 %u "
                "nullDescriptor %u bufferInt64Atomics %u\n",
                VK_API_VERSION_MAJOR(g_caps.api), VK_API_VERSION_MINOR(g_caps.api), g_caps.subgroup_size,
                g_caps.storage_align, g_caps.int8, g_caps.int16, g_caps.int64, g_caps.float64, g_caps.rba,
                g_caps.rba2, g_caps.null_descriptor, g_caps.buffer_int64_atomics);
    std::printf("subgroups: size %u, control %u (min %u max %u, required-size stages %#x), computeFullSubgroups %u, "
                "stages %#x, ops %#x, workgroup_memory_explicit_layout %u\n", g_caps.subgroup_size, g_caps.size_control, g_caps.min_subgroup,
                g_caps.max_subgroup, g_caps.required_size_stages, g_caps.full_subgroups, g_caps.subgroup_stages,
                g_caps.subgroup_ops, g_caps.explicit_layout);
    std::printf("binder guards mirrored: required-span %d, swizzle-group %d, u32-range %d\n",
#ifdef BB_MIRROR_REQUIRED_SPAN
                1,
#else
                0,
#endif
#ifdef BB_MIRROR_SWIZZLE_GROUP
                1,
#else
                0,
#endif
#ifdef BB_MIRROR_U32_RANGE
                1
#else
                0
#endif
    );
    std::printf("vertex fetch: %s\n", VertexPulling() ? "pulled in the shader (Profile::emulate_vertex_fetch)"
                                                      : "fixed-function vertex input (mirrored)");
    const auto flag = [](auto& prof, int which) -> const char* {
        bool present = false, on = false;
        if (which == 0) { if constexpr (requires { prof.emulate_robust_buffer_access; }) { present = true; on = prof.emulate_robust_buffer_access; } }
        if (which == 1) { if constexpr (requires { prof.emulate_wave64; }) { present = true; on = prof.emulate_wave64; } }
        if (which == 2) { if constexpr (requires { prof.emulate_vertex_fetch; }) { present = true; on = prof.emulate_vertex_fetch; } }
        return !present ? "absent" : (on ? "ON" : "off");
    };
    std::printf("profile: null_descriptors %d, robust_buffer_access %s, wave64 %s, vertex_fetch %s; "
                "robustBufferAccess %s\n",
                g_profile.emulate_null_descriptors, flag(g_profile, 0), flag(g_profile, 1), flag(g_profile, 2),
                opt.rba ? "enabled (production)" : "DISABLED (--no-rba)");

    if (opt.bench) {
        return Bench(opt);
    }
    if (!opt.compile_only.empty()) {
        // A real game shader through the production recompiler: compile + spirv-val only.
        FILE* f = std::fopen(opt.compile_only.c_str(), "rb");
        if (!f) { std::perror(opt.compile_only.c_str()); return 2; }
        Template t;
        t.name = "compile-only " + opt.compile_only + " (" + opt.stage + ")";
        u32 w;
        while (std::fread(&w, 4, 1, f) == 1) t.code.push_back(w);
        std::fclose(f);
        t.fragment = opt.stage == "fs";
        t.vertex = opt.stage == "vs";
        t.hash = 0x9e3779b97f4a7c15ull ^ t.code.size();
        for (u32 x : t.code) t.hash = (t.hash ^ x) * 0x100000001b3ull;
        g_templates.push_back(t);
        Job j;
        j.group = "compile-only";
        j.label = t.name;
        j.ud = opt.ud;
        g_jobs.push_back(j);
        CompileAll(opt.outdir);
        ValidateModules(opt.outdir);
        const Job& r = g_jobs[0];
        const bool invalid = g_spirv_invalid.count({0, r.perm});
        std::printf("%s: %s%s\n%s", t.name.c_str(),
                    r.status == Status::Ok ? "compiled" : (r.status == Status::LoudCompile ? "LOUD ABORT" : "CRASH"),
                    r.status == Status::Ok ? (invalid ? ", INVALID SPIR-V" : ", SPIR-V valid") : "",
                    r.status == Status::Ok ? (invalid ? (g_spirv_invalid[{0, r.perm}] + "\n").c_str() : "")
                                           : (r.message + "\n").c_str());
        return r.status == Status::Ok && !invalid ? 0 : 1;
    }
    // Cases
    AddLoadCases();
    AddStoreCases();
    AddAtomicCases();
    AddReuseCases();
    AddWaveCases();
    AddVertexCases();
    AddInternalCases();
    AddSpecReuseCases();
    AddRenderCases();
    AddPushDescriptorCases();
    if (!opt.only.empty()) {
        std::vector<Job> kept;
        for (auto& j : g_jobs) {
            if ((j.group + " " + j.label).find(opt.only) != std::string::npos) kept.push_back(std::move(j));
        }
        g_jobs = std::move(kept);
    }
    for (u32 i = 0; i < g_jobs.size(); ++i) {
        Job& j = g_jobs[i];
        j.slot = i % opt.batch;
        const gcn_vsharp v = ActualV(j);
        const auto t = j.zero_sharp ? std::array<u32, 4>{} : Pack(v);
        gcn_vsharp pv = RawV(ParamSlot);
        pv.base = ParamVa + u64(j.slot) * ParamSlot;
        gcn_vsharp ov = RawV(ResultSlot);
        ov.base = ResultVa + u64(j.slot) * ResultSlot;
        const auto p = Pack(pv), o = Pack(ov);
        for (u32 k = 0; k < 4; ++k) {
            j.ud[k] = t[k];
            j.ud[4 + k] = p[k];
            j.ud[8 + k] = o[k];
        }
        j.ud[12] = j.sgpr_soff;
        j.ud[13] = j.smem_sgpr;
        if (g_templates[j.tmpl].compile_only) {
            j.ud = {};
            for (u32 k = 0; k < 4; ++k) j.ud[4 + k] = t[k];
            j.ud[8] = 0x1234;
            j.ud[9] = 16;
        } else if (g_templates[j.tmpl].srt) {
            std::copy_n(t.begin(), 4, j.vtable.begin());
            j.vtable[4] = 0x5A17C0DEu ^ i;
            const u64 table = reinterpret_cast<u64>(j.vtable.data());
            for (u32 k = 0; k < 4; ++k) j.ud[k] = 0;
            j.ud[2] = u32(table);
            j.ud[3] = u32(table >> 32);
        } else if (g_templates[j.tmpl].vertex) {
            // s[0:1] fetch shader address, s[2:3] V# table address, s[4:7] scratch, s[8:11] results
            std::copy_n(t.begin(), 4, j.vtable.begin());
            const u64 fetch = reinterpret_cast<u64>(g_templates[j.tmpl].fetch.data());
            const u64 table = reinterpret_cast<u64>(j.vtable.data());
            j.ud = {};
            j.ud[0] = u32(fetch);
            j.ud[1] = u32(fetch >> 32);
            j.ud[2] = u32(table);
            j.ud[3] = u32(table >> 32);
            for (u32 k = 0; k < 4; ++k) j.ud[8 + k] = o[k];
        }
    }
    std::printf("cases: %zu jobs over %zu shader templates\n", g_jobs.size(), g_templates.size());

    // Phase 1: compile in child processes (loud aborts are recorded per job).
    CompileAll(opt.outdir);
    size_t perms = g_spirv.size();
    std::printf("compiled: %zu modules\n", perms);
    ValidateModules(opt.outdir);
    if (!opt.dump_spv.empty()) {
        for (const auto& [key, spv] : g_spirv) {
            const std::string f = opt.dump_spv + "/t" + std::to_string(key.first) + "-p" + std::to_string(key.second) + ".spv";
            if (FILE* fp = std::fopen(f.c_str(), "wb")) {
                std::fwrite(spv.data(), 4, spv.size(), fp);
                std::fclose(fp);
            }
        }
        FILE* idx = std::fopen((opt.dump_spv + "/templates.txt").c_str(), "w");
        for (u32 i = 0; idx && i < g_templates.size(); ++i) std::fprintf(idx, "t%u %s\n", i, g_templates[i].name.c_str());
        if (idx) std::fclose(idx);
    }

    // Phase 2: run on the device.
    Gpu g;
    InitGpu(g, opt.rba);
    std::map<u32, JobResult> results;
    for (u32 first = 0; first < g_jobs.size(); first += opt.batch) {
        std::vector<u32> batch;
        for (u32 i = first; i < std::min<u32>(first + opt.batch, g_jobs.size()); ++i) batch.push_back(i);
        auto r = RunBatch(g, batch);
        for (auto& [k, v] : r) results[k] = std::move(v);
    }
    // Re-run failing store/atomic jobs alone, so writes by other jobs cannot be misattributed.
    if (opt.batch > 1) {
        for (auto& [ji, r] : results) {
            const auto kind = g_templates[g_jobs[ji].tmpl].op.kind;
            if (r.verdict != Verdict::Fail || (kind != GCN_STORE && kind != GCN_ATOMIC)) continue;
            auto alone = RunBatch(g, {ji});
            if (alone.count(ji)) r = alone[ji];
        }
    }
    for (auto& [ji, r] : results) {
        if (g_jobs[ji].force_push && r.verdict == Verdict::Fail) {
            r.verdict = Verdict::KnownMoltenVk;
            r.detail = "  MoltenVK OpArrayLength under push descriptors (renderer uses allocated sets on Apple)\n" + r.detail;
        }
    }
    // Compile-only jobs: a valid shader must compile; an abort blocks the game.
    for (u32 ji = 0; ji < g_jobs.size(); ++ji) {
        const Job& j = g_jobs[ji];
        if (!g_templates[j.tmpl].compile_only) continue;
        if (j.status == Status::Ok && g_templates[j.tmpl].check_bindings) {
            // OpDecorate (71) ... Binding (33) N in the module this job would use
            const auto& spv = g_spirv.at({j.tmpl, j.perm});
            std::vector<u32> got;
            for (size_t w = 5; w < spv.size();) {
                const u32 count = spv[w] >> 16, opcode = spv[w] & 0xFFFF;
                if (count == 0) break;
                if (opcode == 71 && count >= 4 && spv[w + 2] == 33) got.push_back(spv[w + 3]);
                w += count;
            }
            std::sort(got.begin(), got.end());
            std::vector<u32> want;
            for (u32 b = 0; b < j.binds.size(); ++b) want.push_back(j.start_unified + b);
            auto list = [](const std::vector<u32>& v) {
                std::string r;
                for (u32 x : v) r += (r.empty() ? "" : ",") + std::to_string(x);
                return "{" + r + "}";
            };
            results[ji] = got == want ? JobResult{Verdict::Strict, ""}
                                      : JobResult{Verdict::Fail, "  module perm " + std::to_string(j.perm) +
                                                                     " declares bindings " + list(got) +
                                                                     ", the pipeline layout expects " + list(want) +
                                                                     " (reused across start bindings)\n"};
        } else if (g_templates[j.tmpl].expect_loud) {
            results[ji] = j.status == Status::LoudCompile
                              ? JobResult{Verdict::Strict, ""}
                              : JobResult{Verdict::Fail, "  compiled, but must be rejected (workgroup-divergent "
                                                         "barrier would deadlock)\n"};
        } else if (j.status == Status::Ok) {
            results[ji] = {Verdict::Strict, ""};
        } else if (j.status == Status::LoudCompile) {
            results[ji] = {Verdict::Fail, "  aborts while compiling a valid shader: " + j.message + "\n"};
        }
    }
    // A job whose module fails spirv-val is not a pass, whatever it returned.
    for (auto& [ji, r] : results) {
        const Job& j = g_jobs[ji];
        auto it = g_spirv_invalid.find({j.tmpl, j.perm});
        if (j.status != Status::Ok || it == g_spirv_invalid.end()) continue;
        r.detail = "  invalid SPIR-V (" + std::string(VerdictName(r.verdict)) + " on MoltenVK): " + it->second + "\n" + r.detail;
        r.verdict = Verdict::InvalidSpirv;
    }
    // Loud classification for jobs that did not run.
    for (u32 ji = 0; ji < g_jobs.size(); ++ji) {
        Job& j = g_jobs[ji];
        if (results.count(ji) && j.status == Status::Ok) continue;
        JobResult r;
        switch (j.status) {
        case Status::LoudCompile:
        case Status::LoudBinder:
        case Status::LoudPipeline: {
            // Every Apple binder guard mirrored here is an orchestrator-approved loud check.
            const bool approved = j.approved_struct_soff || j.approved_swizzle || j.isa_invalid || j.approved_offen_only ||
                                  g_templates[j.tmpl].wave_divergent ||
                                  j.status == Status::LoudBinder;
            r.verdict = LoudRequired(g, j) ? Verdict::LoudRequired
                                           : (approved ? Verdict::LoudApproved : Verdict::LoudUnneeded);
            break;
        }
        case Status::Crash: r.verdict = Verdict::Crash; break;
        default: r.verdict = Verdict::HarnessError; break;
        }
        r.detail = "  " + j.message + "\n";
        if (!results.count(ji) || results[ji].verdict == Verdict::HarnessError) results[ji] = r;
    }

    for (u32 ji = 0; ji < g_jobs.size(); ++ji) {
        if (g_templates[g_jobs[ji].tmpl].helper && g_jobs[ji].status == Status::Ok) results.erase(ji);
    }
    // Report
    std::map<Verdict, u32> totals;
    std::map<std::string, std::map<Verdict, u32>> by_group;
    for (auto& [ji, r] : results) {
        totals[r.verdict]++;
        by_group[g_jobs[ji].group][r.verdict]++;
    }
    const std::string path = opt.outdir + "/report-" + opt.label + ".txt";
    FILE* rep = std::fopen(path.c_str(), "w");
    auto both = [&](const char* fmt, auto... args) {
        std::printf(fmt, args...);
        if (rep) std::fprintf(rep, fmt, args...);
    };
    both("\n== GCN2 buffer conformance [%s] ==\n", opt.label.c_str());
    for (auto& [grp, m] : by_group) {
        std::string line;
        for (auto& [v, n] : m) line += std::string(" ") + VerdictName(v) + "=" + std::to_string(n);
        both("  %-18s%s\n", grp.c_str(), line.c_str());
    }
    std::string line;
    for (auto& [v, n] : totals) line += std::string(" ") + VerdictName(v) + "=" + std::to_string(n);
    both("  %-18s%s\n", "TOTAL", line.c_str());
    u32 shown = 0;
    for (auto& [ji, r] : results) {
        if (r.verdict == Verdict::Strict) continue;
        const Job& j = g_jobs[ji];
        const std::string head = std::string(VerdictName(r.verdict)) + " #" + std::to_string(ji) + " [" + j.group +
                                 "] " + j.label + " (perm " + std::to_string(j.perm) + ")\n";
        if (rep) std::fprintf(rep, "%s%s", head.c_str(), r.detail.c_str());
        const bool important = r.verdict == Verdict::Fail || r.verdict == Verdict::InvalidSpirv || r.verdict == Verdict::Crash ||
                               r.verdict == Verdict::HarnessError;
        if (important && shown < opt.max_detail) {
            std::printf("%s%s", head.c_str(), r.detail.c_str());
            ++shown;
        }
    }
    if (rep) std::fclose(rep);
    std::printf("report: %s\n", path.c_str());
    const bool bad = totals[Verdict::Fail] || totals[Verdict::InvalidSpirv] || totals[Verdict::Crash] ||
                     totals[Verdict::HarnessError];
    return bad ? 1 : 0;
}
