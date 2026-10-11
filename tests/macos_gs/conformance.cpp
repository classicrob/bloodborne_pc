// SPDX-License-Identifier: GPL-2.0-or-later
// Geometry emulation (Apple) runtime conformance. Synthetic GCN ES, GS and copy shaders go through
// the production recompiler in their emulation roles (EsCompute, GsCompute, CopyVertex) and run on
// MoltenVK the way Rasterizer::RecordGsEmulation records them (mirrored here: scratch layout and
// header, ES dispatch, barrier, GS dispatch, barrier, indexed strip draw with restart). Checked
// against a CPU model of the GS semantics:
//  - the index runs, exactly (slot order, emitted vertices, restart after strips that draw, rewind
//    of short strips, emits past VGT_GS_MAX_VERT_OUT dropped, restart padding);
//  - the GSVS record of every emitted vertex, exactly;
//  - the image, bit-exactly, against the same primitives drawn as a plain list (flat attribute ->
//    provoking vertex; constant-alpha blending -> primitive order).
// The GS emits count = p & 7 vertices of primitive p (divergent across lanes) and cuts after
// vertex e when bit e of p >> 3 is set. Primitives share four rows (p & 3), so they overlap and
// the blended image depends on their order.
// Stream-out cases (points out): the copy shader also stores each vertex's param0 to stream-out
// buffer 0 as Gnm's copy shaders do (V# from its s[2:3] table, lanes below the count in s4, index
// s5 + lane by add_tid, base offset s6; raw dwords or typed xyzw). That runs as the CopyStreamOut
// kernel after the compaction kernel's stream-out tail, as RecordGsStreamOut records it; the
// buffer, the advanced offset, the overflow counts and SoCount are checked against a CPU model
// (the leading vertices that fit, in output order).
//
//   conformance SPV_DIR [--only SUBSTRING]
//               [--control base-vertex|reverse-order|rotate-provoking|so-lane]
//   conformance SPV_DIR --provoking [--control provoking-last]
//       the device's flat (provoking) vertex per topology (RunProvokingCase)
// The controls perturb one side on purpose; every case that draws something must then FAIL,
// showing that the data, order and provoking-vertex checks can fail.
#define VK_ENABLE_BETA_EXTENSIONS // VK_KHR_portability_subset (triangle fans)
#include <vulkan/vulkan.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <numeric>
#include <string>
#include <vector>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/gs_emulation.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/runtime_info.h"
#include "tests/macos_gs/gs_asm.h"

namespace GsEmu = Shader::GsEmulation;

#define CHECK(call)                                                                                \
    do {                                                                                           \
        const VkResult r_ = (call);                                                                \
        if (r_ != VK_SUCCESS) {                                                                    \
            std::fprintf(stderr, "%s: Vulkan result %d\n", #call, r_);                             \
            std::exit(2);                                                                          \
        }                                                                                          \
    } while (0)
#define REQUIRE(c)                                                                                 \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            std::fprintf(stderr, "Requirement failed: %s\n", #c);                                  \
            std::exit(2);                                                                          \
        }                                                                                          \
    } while (0)

namespace {

std::string g_control; // negative control (see the usage above)

constexpr u32 Size = 128;           // render target
constexpr u32 OutDwords = 8;        // GSVS vertex: pos0 + param0
constexpr u32 EsDwords = 4;         // ESGS record: vertex id, instance id, 0, 1
constexpr u32 Iterations = 8;       // unrolled emit attempts (count = p & 7)
constexpr u32 BaseVertex = 7;
constexpr float PointSizePx = 3.0f; // point output: PA_SU_POINT_SIZE (list.vert writes it too)

enum class Index { None, U16, U32 };
// RawBallot: Raw, stored only by lanes whose rank among the active lanes (v_mbcnt of exec read as
// an SGPR pair: a 64-lane ballot across both Apple subgroups) equals their lane.
enum class SoStore { None, Raw, Typed, RawBallot };

struct Case {
    const char* name;
    AmdGpu::PrimitiveType in_prim;
    AmdGpu::GsOutputPrimitiveType out_prim;
    u32 max_vert_out;
    u32 records;
    u32 instances;
    Index index;
    u32 index_skip_bytes; // guest index data starts this far into the bound range
    // Indirect draw: the production setup kernel (gs_indirect_setup.comp) reads a
    // VkDraw[Indexed]IndirectCommand; indexed ones use firstIndex = IndirectFirst.
    bool indirect = false;
    bool overflow = false; // scratch too small: must draw nothing and report
    // Stream-out to buffer 0 (points out only).
    SoStore so_store = SoStore::None;
    u32 so_room = 0;       // vertices that fit after so_start (VGT_STRMOUT_BUFFER_SIZE_0)
    u32 so_start = 0;      // VGT_STRMOUT_BUFFER_OFFSET_0 (dwords) before the first draw
    u32 so_draws = 1;      // the same draw this many times in a row (offset continuation)
    bool so_reset = false; // STRMOUT_BUFFER_UPDATE of the offset to 0 between the draws
    bool so_tight = false; // the V# and its binding end exactly at VGT_STRMOUT_BUFFER_SIZE
};
constexpr u32 SoStrideDw = 4; // VGT_STRMOUT_VTX_STRIDE_0: param0
constexpr u32 SoCanary = 0xABABABABu;
constexpr u32 IndirectFirst = 3;

/// Push constants of gs_indirect_setup.comp (vk_gs_emulation.cpp SetupParams).
struct SetupParams {
    u32 args_skip, indexed, index_kind, index_skip, index_count;
    u32 verts_per_prim, invocations, max_vert_out, esgs_dwords, vertex_dwords, run_length;
    u32 capacity_dwords;
    u32 args_address_lo, args_address_hi;
    u32 point_output;
};

u32 VertsPerPrim(AmdGpu::PrimitiveType t) {
    return t == AmdGpu::PrimitiveType::PointList ? 1 : t == AmdGpu::PrimitiveType::LineList ? 2 : 3;
}

// ---------------------------------------------------------------------------------------------
// Synthetic GCN programs.

std::vector<u32> EsProgram() {
    using namespace GsAsm;
    Code c;
    c.VCvtF32U32(2, 0); // vertex id (VertexId incl. base vertex)
    c.VCvtF32U32(3, 1); // instance id (step rate 1)
    c.VMov(4, Inline(0));
    c.VMov(5, InlineF1);
    for (u32 d = 0; d < EsDwords; ++d) {
        c.RingDword(true, 2 + d, 0, false, d * 4, Inline(0));
    }
    c.SEndpgm();
    return c.words;
}

float PosX(u32 e) {
    return -1.0f + float(e) * 0.125f;
}
float PosYBase(u32 e) {
    return -1.0f + float(e & 1) * 0.0625f;
}

std::vector<u32> GsProgram(const Case& k) {
    using namespace GsAsm;
    const u32 n = k.max_vert_out;
    const u32 last = VertsPerPrim(k.in_prim) - 1;
    // Input vertex registers: v0 = vertex 0, v1 = vertex 1, v3 = vertex 2 (translate.cpp).
    const u32 last_reg = last == 0 ? 0 : last == 1 ? 1 : 3;
    Code c;
    c.VLshlrevB32(10, Inline(2), 0);        // vertex 0 offset
    c.VLshlrevB32(14, Inline(2), last_reg); // last vertex offset
    c.SMovLiteral(12, 0 * 256);             // bucket 0: ESGS dword 0
    c.SMovLiteral(13, 1 * 256);             // bucket 1: ESGS dword 1
    c.RingDword(false, 11, 10, true, 0, 12); // v11 = vertex 0 id
    c.RingDword(false, 12, 10, true, 0, 13); // v12 = vertex 0 instance
    c.RingDword(false, 15, 14, true, 0, 12); // v15 = last vertex id
    c.SWaitcnt();
    c.VAndB32Literal(20, 7, 2);             // count = p & 7
    c.VLshrrevB32(21, Inline(3), 2);        // cut mask = p >> 3
    c.VCvtF32U32(22, 2);                    // float(p)
    c.VAndB32Literal(24, 3, 2);             // row = p & 3: primitives overlap, so order shows
    c.VCvtF32U32(25, 24);
    for (u32 e = 0; e < Iterations; ++e) {
        c.VCmpLtU32(Inline(e), 20);         // e < count
        c.SAndSaveexecB64(20, VCC_LO);
        const size_t skip_emit = c.SCbranchExecz();
        c.VMovLiteral(30, FloatBits(PosX(e)));
        c.VMulF32Literal(31, 0.0625f, 25);
        c.VAddF32Literal(31, PosYBase(e), 31);
        c.VMovLiteral(32, FloatBits(0.5f));
        c.VMov(33, InlineF1);
        c.VMulF32Literal(34, 1.0f / 64.0f, 22);
        c.VMovLiteral(35, FloatBits(float(e) / 8.0f));
        c.VMov(36, Vgpr(12));
        c.VMulF32Literal(37, 1.0f / 256.0f, 15);
        const u32 vertex_term = std::min(e, n - 1) * 4;
        for (u32 comp = 0; comp < OutDwords; ++comp) {
            c.RingDword(true, 30 + comp, 0, false, comp * n * 4 + vertex_term, Inline(0));
        }
        c.SSendmsgGs(2); // emit
        c.VAndB32Literal(23, 1u << e, 21);
        c.VCmpNeU32(Inline(0), 23);
        c.SAndSaveexecB64(24, VCC_LO);
        const size_t skip_cut = c.SCbranchExecz();
        c.SSendmsgGs(1); // cut
        c.PatchBranch(skip_cut);
        c.SMovB64(EXEC_LO, 24);
        c.PatchBranch(skip_emit);
        c.SMovB64(EXEC_LO, 20);
    }
    c.SSendmsgGsDone();
    c.SEndpgm();
    return c.words;
}

std::vector<u32> CopyProgram(const Case& k) {
    using namespace GsAsm;
    const bool so = k.so_store != SoStore::None;
    Code c;
    c.SMovLiteral(VCC_HI, 0); // the Gnm copy-shader signature (s_mov_b32 vcc_hi, #imm)
    if (so) {
        // As GXFlver_*_SO: the stream-out V# from the s[2:3] table, the base offset in bytes,
        // the vertices of this wave that fit (s4 bits 16-22).
        c.SLoadDwordx4(16, 2, 0);
        c.SLshlB32(6, 6, Inline(2));
        c.SBfeU32(11, 4, 16, 7);
    }
    const u32 soffset = so ? 8 : 4; // s4.. are the stream-out inputs
    for (u32 comp = 0; comp < OutDwords; ++comp) {
        c.words.push_back(0xB0000000u | (soffset << 16) | (comp * k.max_vert_out * 64)); // s_movk
        c.RingDword(false, 1 + comp, 0, true, 0, soffset);
    }
    c.SWaitcnt();
    c.Exp(12, false, 1);  // pos0
    c.Exp(32, true, 5);   // param0
    if (so) {
        c.LaneId(40);
        c.VMov(41, 11);
        c.VCmpLtU32(Vgpr(40), 41); // lane < vertices that fit
        c.SAndSaveexecB64(20, VCC_LO);
        c.VMov(42, 5);             // write index; the V#'s add_tid adds the lane
        if (k.so_store == SoStore::RawBallot) {
            c.SMovB64(22, EXEC_LO);    // the wave's active lanes
            c.MaskRank(43, 22);
            c.VCmpEqU32(Vgpr(43), 40); // rank == lane: true for every active lane
            c.SAndSaveexecB64(24, VCC_LO);
        }
        if (k.so_store != SoStore::Typed) {
            for (u32 comp = 0; comp < 4; ++comp) {
                c.BufferStoreDword(5 + comp, 42, true, comp * 4, 6, 16);
            }
        } else {
            c.TBufferStore(7, 14, 7, 5, 42, true, 0, 6, 16); // xyzw, 32_32_32_32 float
        }
        c.SOrSaveexecB64(20, 20); // as GXFlver_*_SO: restore exec (s_or_saveexec_b64)
    }
    c.SEndpgm();
    return c.words;
}

// ---------------------------------------------------------------------------------------------
// Production recompiler.

/// VkPhysicalDeviceVulkan11Properties::subgroupSupportedStages, set by InitGpu: the production
/// profile takes it from the device (MoltenVK has none in vertex functions), which decides
/// whether a vertex stage's wave operations are lowered without subgroups.
u32 g_subgroup_stages = 0;

Shader::Profile AppleProfile() {
    Shader::Profile p{};
    REQUIRE(g_subgroup_stages != 0); // InitGpu first
    p.subgroup_supported_stages = g_subgroup_stages;
    p.max_viewport_width = 16384;
    p.max_viewport_height = 16384;
    p.max_shared_memory_size = 32768;
    p.supported_spirv = 0x00010600;
    p.subgroup_size = 32;
    p.support_int8 = true;
    p.support_int16 = true;
    p.support_int64 = true;
    p.support_float16 = true;
    p.support_fp16_signed_zero_inf_nan_preserve = true;
    p.support_fp32_signed_zero_inf_nan_preserve = true;
    p.needs_buffer_offsets = true;
    p.emulate_null_descriptors = true;
    p.emulate_robust_buffer_access = true;
    p.emulate_vertex_fetch = true;
    p.repair_mul_signed_zero = true;
    p.emulate_wave64 = true;
    p.emulate_geometry_stage = true;
    return p;
}

struct Compiled {
    std::vector<u32> spv;
    std::vector<Shader::BufferType> buffers;
    std::vector<u32> flat; ///< the flattened user data its Flatbuf binding holds
};

Compiled Compile(const std::vector<u32>& code, Shader::RuntimeInfo rt,
                 const std::array<u32, 16>& ud = {}) {
    static Shader::Pools pools;
    const auto profile = AppleProfile();
    const Shader::ShaderParams params{.user_data = std::span<const u32, 16>(ud),
                                      .code = std::span<const u32>(code),
                                      .hash = 0x6500 + u64(rt.gs_role)};
    Shader::Info info(rt.hw_stage, rt.sw_stage, params);
    Shader::Backend::Bindings binding{};
    const auto program = Shader::TranslateProgram(code, pools, info, rt, profile);
    Compiled out{Shader::Backend::SPIRV::EmitSPIRV(profile, rt, program, binding), {},
                 info.flattened_ud_buf};
    for (const auto& b : info.buffers) {
        out.buffers.push_back(b.buffer_type);
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// CPU model of the GS semantics and of the emulation's output layout.

struct Vertex {
    std::array<float, OutDwords> data;
};

struct Expected {
    std::vector<u32> runs;                 // index runs of all slots
    std::vector<std::pair<u32, Vertex>> records; // emitted vertices: (record index, data)
    std::vector<Vertex> list;              // reference primitives as a plain list
    std::vector<u32> compact;              // point output: the compacted index list
};

Expected Model(const Case& k, const std::vector<u32>& vertex_ids) {
    const u32 n = k.max_vert_out;
    const u32 per = VertsPerPrim(k.in_prim);
    const u32 prims = k.records / per;
    const u32 m = GsEmu::MinStripVertices(k.out_prim);
    const bool points = GsEmu::PointOutput(k.out_prim);
    const u32 run_length = GsEmu::IndexRunLength(n, k.out_prim);
    Expected x;
    for (u32 inst = 0; inst < k.instances; ++inst) {
        for (u32 p = 0; p < prims; ++p) {
            const u32 slot = inst * prims + p;
            const u32 count = p & 7, cuts = p >> 3;
            std::vector<u32> run;
            u32 strip_start = 0, strip_len = 0, emitted = 0;
            std::vector<std::vector<Vertex>> strips(1);
            const float vid0 = float(vertex_ids[p * per]);
            const float vid_last = float(vertex_ids[p * per + per - 1]);
            for (u32 e = 0; e < Iterations; ++e) {
                if (e >= count) {
                    continue;
                }
                if (emitted < n) {
                    Vertex v{};
                    v.data = {PosX(e), float(p & 3) * 0.0625f + PosYBase(e), 0.5f, 1.0f,
                              float(p) / 64.0f, float(e) / 8.0f, float(inst), vid_last / 256.0f};
                    x.records.push_back({slot * n + emitted, v});
                    run.push_back(slot * n + emitted);
                    strips.back().push_back(v);
                    ++emitted;
                    ++strip_len;
                }
                if (!points && (cuts >> e) & 1) { // a cut ends nothing for points
                    if (strip_len >= m) {
                        run.push_back(GsEmu::RestartIndex);
                    } else {
                        run.resize(strip_start);
                    }
                    strip_start = u32(run.size());
                    strip_len = 0;
                    strips.emplace_back();
                }
            }
            (void)vid0;
            if (strip_len < m) {
                run.resize(strip_start);
            }
            REQUIRE(run.size() <= run_length);
            run.resize(run_length, GsEmu::RestartIndex);
            for (const u32 index : run) {
                if (points && index != GsEmu::RestartIndex) {
                    x.compact.push_back(index);
                }
            }
            x.runs.insert(x.runs.end(), run.begin(), run.end());
            // Strip assembly (Vulkan order; the provoking vertex is the first of each primitive).
            for (const auto& s : strips) {
                if (points) {
                    x.list.insert(x.list.end(), s.begin(), s.end());
                } else if (m == 3) {
                    for (size_t t = 0; t + 2 < s.size(); ++t) {
                        std::array<Vertex, 3> tri{s[t], s[t + 1 + t % 2], s[t + 2 - t % 2]};
                        if (g_control == "rotate-provoking") {
                            tri = {tri[1], tri[2], tri[0]}; // same winding, other provoking vertex
                        }
                        x.list.insert(x.list.end(), tri.begin(), tri.end());
                    }
                } else {
                    for (size_t t = 0; t + 1 < s.size(); ++t) {
                        x.list.push_back(s[t]);
                        x.list.push_back(s[t + 1]);
                    }
                }
            }
        }
    }
    if (g_control == "reverse-order") {
        const size_t per_prim = GsEmu::MinStripVertices(k.out_prim) == 3 ? 3
                                : GsEmu::PointOutput(k.out_prim)          ? 1
                                                                          : 2;
        std::vector<Vertex> reversed;
        for (size_t i = x.list.size(); i >= per_prim; i -= per_prim) {
            reversed.insert(reversed.end(), x.list.begin() + (i - per_prim), x.list.begin() + i);
        }
        x.list = std::move(reversed);
    }
    return x;
}

// ---------------------------------------------------------------------------------------------
// Vulkan.

struct Gpu {
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    VkQueue queue;
    VkPhysicalDeviceMemoryProperties memory;
    VkPhysicalDeviceProperties props;
    VkCommandPool pool;
    VkCommandBuffer cmd;
    VkFence fence;
};
Gpu g;

std::vector<u32> ReadFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    REQUIRE(f.good());
    std::vector<char> b((std::istreambuf_iterator<char>(f)), {});
    std::vector<u32> w(b.size() / 4);
    std::memcpy(w.data(), b.data(), w.size() * 4);
    return w;
}

void InitGpu() {
    const VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "gs conformance", 0,
                                nullptr, 0, VK_API_VERSION_1_3};
    const char* ext = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
    const VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, nullptr,
                                   VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR, &app, 0,
                                   nullptr, 1, &ext};
    CHECK(vkCreateInstance(&ici, nullptr, &g.instance));
    u32 count = 1;
    vkEnumeratePhysicalDevices(g.instance, &count, &g.physical);
    vkGetPhysicalDeviceMemoryProperties(g.physical, &g.memory);
    vkGetPhysicalDeviceProperties(g.physical, &g.props);
    VkPhysicalDeviceVulkan11Properties p11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &p11};
    vkGetPhysicalDeviceProperties2(g.physical, &p2);
    g_subgroup_stages = p11.subgroupSupportedStages;
    std::printf("device subgroup stages %#x\n", g_subgroup_stages);
    // Enable everything the device has (the recompiler's capabilities follow the profile).
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13};
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &f12};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f11};
    vkGetPhysicalDeviceFeatures2(g.physical, &f2);
    f2.features.robustBufferAccess = VK_FALSE;
    const float priority = 1.0f;
    const VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, 0, 1,
                                      &priority};
    const char* dext = "VK_KHR_portability_subset";
    const VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f2, 0, 1, &qci, 0, nullptr,
                                 1, &dext, nullptr};
    CHECK(vkCreateDevice(g.physical, &dci, nullptr, &g.device));
    vkGetDeviceQueue(g.device, 0, 0, &g.queue);
    const VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr,
                                      VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, 0};
    CHECK(vkCreateCommandPool(g.device, &cpi, nullptr, &g.pool));
    const VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr,
                                          g.pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    CHECK(vkAllocateCommandBuffers(g.device, &cai, &g.cmd));
    const VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    CHECK(vkCreateFence(g.device, &fci, nullptr, &g.fence));
}

u32 MemoryType(u32 bits, VkMemoryPropertyFlags flags) {
    for (u32 i = 0; i < g.memory.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) && (g.memory.memoryTypes[i].propertyFlags & flags) == flags) {
            return i;
        }
    }
    REQUIRE(!"memory type");
    return 0;
}

struct Buffer {
    VkBuffer buffer;
    VkDeviceMemory memory;
    u8* data;
    u64 size;
};

Buffer MakeBuffer(u64 size) {
    Buffer b{};
    b.size = size;
    const VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0, size,
                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                     VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                     VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                     VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                 VK_SHARING_MODE_EXCLUSIVE};
    CHECK(vkCreateBuffer(g.device, &bci, nullptr, &b.buffer));
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(g.device, b.buffer, &mr);
    const VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, mr.size,
                                  MemoryType(mr.memoryTypeBits,
                                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    CHECK(vkAllocateMemory(g.device, &ai, nullptr, &b.memory));
    CHECK(vkBindBufferMemory(g.device, b.buffer, b.memory, 0));
    CHECK(vkMapMemory(g.device, b.memory, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(&b.data)));
    std::memset(b.data, 0, size);
    return b;
}

void FreeBuffer(Buffer& b) {
    vkDestroyBuffer(g.device, b.buffer, nullptr);
    vkFreeMemory(g.device, b.memory, nullptr);
}

VkShaderModule Module(const std::vector<u32>& spv) {
    const VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0,
                                      spv.size() * 4, spv.data()};
    VkShaderModule m;
    CHECK(vkCreateShaderModule(g.device, &ci, nullptr, &m));
    return m;
}

/// One descriptor set of storage buffers (binding i = range i), push constants of PushData size.
struct Layout {
    VkDescriptorSetLayout set_layout;
    VkPipelineLayout layout;
    VkDescriptorPool pool;
    VkDescriptorSet set;
};

Layout MakeLayout(VkShaderStageFlags stages, const std::vector<VkDescriptorBufferInfo>& ranges) {
    Layout l{};
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    for (u32 i = 0; i < ranges.size(); ++i) {
        bindings.push_back({i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, stages, nullptr});
    }
    const VkDescriptorSetLayoutCreateInfo sli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                              nullptr, 0, u32(bindings.size()), bindings.data()};
    CHECK(vkCreateDescriptorSetLayout(g.device, &sli, nullptr, &l.set_layout));
    const VkPushConstantRange push{stages, 0, u32(sizeof(Shader::PushData))};
    const VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0,
                                         1, &l.set_layout, 1, &push};
    CHECK(vkCreatePipelineLayout(g.device, &pli, nullptr, &l.layout));
    const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                    std::max<u32>(1, u32(ranges.size()))};
    const VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0,
                                         1, 1, &size};
    CHECK(vkCreateDescriptorPool(g.device, &dpi, nullptr, &l.pool));
    const VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr,
                                          l.pool, 1, &l.set_layout};
    CHECK(vkAllocateDescriptorSets(g.device, &dai, &l.set));
    std::vector<VkWriteDescriptorSet> writes;
    for (u32 i = 0; i < ranges.size(); ++i) {
        writes.push_back({VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, l.set, i, 0, 1,
                          VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &ranges[i], nullptr});
    }
    vkUpdateDescriptorSets(g.device, u32(writes.size()), writes.data(), 0, nullptr);
    return l;
}

VkPipeline ComputePipeline(VkShaderModule m, VkPipelineLayout layout) {
    const VkComputePipelineCreateInfo ci{
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, nullptr, 0,
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT,
         m, "main", nullptr},
        layout, VK_NULL_HANDLE, 0};
    VkPipeline p;
    CHECK(vkCreateComputePipelines(g.device, VK_NULL_HANDLE, 1, &ci, nullptr, &p));
    return p;
}

VkPipeline GraphicsPipeline(VkShaderModule vs, VkShaderModule fs, VkPipelineLayout layout,
                            VkPrimitiveTopology topology, bool restart, bool vertex_input) {
    const VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT,
         vs, "main", nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
         VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", nullptr}};
    const VkVertexInputBindingDescription binding{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    const VkVertexInputAttributeDescription attributes[2] = {
        {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0}, {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 16}};
    const VkPipelineVertexInputStateCreateInfo vi{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, nullptr, 0,
        vertex_input ? 1u : 0u, &binding, vertex_input ? 2u : 0u, attributes};
    const VkPipelineInputAssemblyStateCreateInfo ia{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, nullptr, 0, topology,
        restart ? VK_TRUE : VK_FALSE};
    const VkViewport viewport{0, 0, float(Size), float(Size), 0, 1};
    const VkRect2D scissor{{0, 0}, {Size, Size}};
    const VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                                nullptr, 0, 1, &viewport, 1, &scissor};
    const VkPipelineRasterizationStateCreateInfo rs{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, nullptr, 0, VK_FALSE, VK_FALSE,
        VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, VK_FALSE, 0, 0, 0,
        1.0f};
    const VkPipelineMultisampleStateCreateInfo ms{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, nullptr, 0, VK_SAMPLE_COUNT_1_BIT};
    // Order-dependent blending: dst = 0.5 * src + 0.5 * dst.
    const VkPipelineColorBlendAttachmentState blend{
        VK_TRUE, VK_BLEND_FACTOR_CONSTANT_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA,
        VK_BLEND_OP_ADD, VK_BLEND_FACTOR_CONSTANT_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA,
        VK_BLEND_OP_ADD, 0xF};
    const VkPipelineColorBlendStateCreateInfo cb{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, nullptr, 0, VK_FALSE,
        VK_LOGIC_OP_COPY, 1, &blend, {0.5f, 0.5f, 0.5f, 0.5f}};
    const VkFormat format = VK_FORMAT_R32G32B32A32_SFLOAT;
    const VkPipelineRenderingCreateInfo rci{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                            nullptr, 0, 1, &format};
    const VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rci, 0,
                                          2, stages, &vi, &ia, nullptr, &vps, &rs, &ms, nullptr,
                                          &cb, nullptr, layout, VK_NULL_HANDLE, 0};
    VkPipeline p;
    CHECK(vkCreateGraphicsPipelines(g.device, VK_NULL_HANDLE, 1, &ci, nullptr, &p));
    return p;
}

struct Target {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
};

Target MakeTarget() {
    Target t{};
    const VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, nullptr, 0, VK_IMAGE_TYPE_2D,
                                VK_FORMAT_R32G32B32A32_SFLOAT, {Size, Size, 1}, 1, 1,
                                VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
                                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                VK_SHARING_MODE_EXCLUSIVE};
    CHECK(vkCreateImage(g.device, &ici, nullptr, &t.image));
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(g.device, t.image, &mr);
    const VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, mr.size,
                                  MemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    CHECK(vkAllocateMemory(g.device, &ai, nullptr, &t.memory));
    CHECK(vkBindImageMemory(g.device, t.image, t.memory, 0));
    const VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, nullptr, 0, t.image,
                                    VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R32G32B32A32_SFLOAT, {},
                                    {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    CHECK(vkCreateImageView(g.device, &vci, nullptr, &t.view));
    return t;
}

void Barrier(VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
             VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
    const VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, nullptr, src_stage, src_access,
                              dst_stage, dst_access};
    const VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO, nullptr, 0, 1, &mb};
    vkCmdPipelineBarrier2(g.cmd, &di);
}

void ImageBarrier(VkImage image, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 src,
                  VkAccessFlags2 src_access, VkPipelineStageFlags2 dst, VkAccessFlags2 dst_access) {
    const VkImageMemoryBarrier2 ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, nullptr, src, src_access,
                                   dst, dst_access, from, to, VK_QUEUE_FAMILY_IGNORED,
                                   VK_QUEUE_FAMILY_IGNORED, image,
                                   {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    const VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO, nullptr, 0, 0, nullptr, 0, nullptr,
                              1, &ib};
    vkCmdPipelineBarrier2(g.cmd, &di);
}

/// Clears the target, runs `draw` in one dynamic-rendering pass, copies the image to `readback`.
template <typename Draw>
void Render(const Target& t, const Buffer& readback, Draw&& draw) {
    ImageBarrier(t.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    const VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, nullptr,
                                          t.view, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                          VK_RESOLVE_MODE_NONE, VK_NULL_HANDLE,
                                          VK_IMAGE_LAYOUT_UNDEFINED, VK_ATTACHMENT_LOAD_OP_CLEAR,
                                          VK_ATTACHMENT_STORE_OP_STORE, {}};
    const VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO, nullptr, 0, {{0, 0}, {Size, Size}},
                             1, 0, 1, &color, nullptr, nullptr};
    vkCmdBeginRendering(g.cmd, &ri);
    draw();
    vkCmdEndRendering(g.cmd);
    ImageBarrier(t.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                 VK_ACCESS_2_TRANSFER_READ_BIT);
    const VkBufferImageCopy region{0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0},
                                   {Size, Size, 1}};
    vkCmdCopyImageToBuffer(g.cmd, t.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1,
                           &region);
}

void Begin() {
    CHECK(vkResetCommandBuffer(g.cmd, 0));
    const VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
                                      VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    CHECK(vkBeginCommandBuffer(g.cmd, &bi));
}

void Submit() {
    CHECK(vkEndCommandBuffer(g.cmd));
    const VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 0, nullptr, nullptr, 1, &g.cmd};
    CHECK(vkQueueSubmit(g.queue, 1, &si, g.fence));
    CHECK(vkWaitForFences(g.device, 1, &g.fence, VK_TRUE, UINT64_MAX));
    CHECK(vkResetFences(g.device, 1, &g.fence));
}

// ---------------------------------------------------------------------------------------------

bool RunCase(const Case& k, const std::string& spv_dir) {
    const u32 per = VertsPerPrim(k.in_prim);
    const u32 prims = k.records / per;
    const u32 n = k.max_vert_out;
    const u32 run_length = GsEmu::IndexRunLength(n, k.out_prim);

    // Vertex ids the ES sees: the index data (a permutation) + BaseVertex, or position + BaseVertex.
    std::vector<u32> indices(k.records), vertex_ids(k.records);
    for (u32 i = 0; i < k.records; ++i) {
        indices[i] = k.index == Index::None ? i : (k.records - 1 - i) * 3 % 65000;
        vertex_ids[i] = indices[i] + BaseVertex;
    }

    // Programs in their roles.
    Shader::RuntimeInfo es{};
    es.Initialize(Shader::HwStage::Export, Shader::SwStage::Vertex);
    es.gs_role = Shader::GsEmulationRole::EsCompute;
    es.props.num_user_data = 4;
    es.props.num_input_vgprs = 1;
    es.props.num_allocated_vgprs = 64;
    es.sw.vs.step_rate_0 = 1;
    es.hw.es.vertex_data_size = EsDwords;
    Shader::RuntimeInfo gs{};
    gs.Initialize(Shader::HwStage::Geometry, Shader::SwStage::Geometry);
    gs.gs_role = Shader::GsEmulationRole::GsCompute;
    gs.gs_max_vert_out = n;
    gs.gs_vertex_dwords = OutDwords;
    gs.props.num_user_data = 4;
    gs.props.num_allocated_vgprs = 64;
    gs.hw.gs.output_vertices = n;
    gs.hw.gs.num_invocations = 1;
    gs.hw.gs.in_vertex_data_size = EsDwords;
    gs.hw.gs.out_vertex_data_size = OutDwords;
    gs.hw.gs.in_primitive = k.in_prim;
    gs.hw.gs.out_primitive.fill(k.out_prim);
    gs.hw.gs.mode = AmdGpu::GsScenario::ScenarioG;
    const auto copy_code = CopyProgram(k);
    gs.hw.gs.vs_copy = copy_code;
    Shader::RuntimeInfo copy{};
    copy.Initialize(Shader::HwStage::Vertex, Shader::SwStage::Vertex);
    copy.gs_role = Shader::GsEmulationRole::CopyVertex;
    copy.gs_max_vert_out = n;
    copy.gs_vertex_dwords = OutDwords;
    copy.props.num_user_data = 4;
    copy.props.num_allocated_vgprs = 64;
    copy.hw.vs.num_outputs = 0;
    if (GsEmu::PointOutput(k.out_prim)) {
        copy.hw.vs.point_size = PointSizePx; // as BuildRuntimeInfo sets it from PA_SU_POINT_SIZE
    }
    // Stream-out: the copy shader's s[2:3] points at a table whose V# 0 is buffer 0 (16-byte
    // vertices addressed by add_tid; its address only names the binding).
    const bool so = k.so_store != SoStore::None;
    REQUIRE(!so || GsEmu::PointOutput(k.out_prim));
    const u32 so_size_dw = k.so_start + k.so_room * SoStrideDw;
    const u32 so_capacity_dw = so_size_dw + 64 * SoStrideDw + 16; // canary past the size
    alignas(16) static AmdGpu::Buffer so_table[1];
    so_table[0] = {};
    so_table[0].base_address = 0x10000000;
    so_table[0].stride = SoStrideDw * 4;
    so_table[0].num_records = (k.so_tight ? so_size_dw : so_capacity_dw) / SoStrideDw;
    REQUIRE(!k.so_tight || so_size_dw % SoStrideDw == 0);
    so_table[0].add_tid_enable = 1;
    so_table[0].data_format = u32(AmdGpu::DataFormat::Format32_32_32_32);
    so_table[0].num_format = u32(AmdGpu::NumberFormat::Float);
    so_table[0].dst_sel_x = 4;
    so_table[0].dst_sel_y = 5;
    so_table[0].dst_sel_z = 6;
    so_table[0].dst_sel_w = 7;
    std::array<u32, 16> copy_ud{};
    if (so) {
        const u64 table = reinterpret_cast<u64>(&so_table[0]);
        copy_ud[2] = u32(table);
        copy_ud[3] = u32(table >> 32);
        copy.gs_so_buffers = 1;
    }
    Shader::RuntimeInfo so_rt = copy;
    so_rt.Initialize(Shader::HwStage::Vertex, Shader::SwStage::Vertex);
    so_rt.gs_role = Shader::GsEmulationRole::CopyStreamOut;
    so_rt.gs_max_vert_out = n;
    so_rt.gs_vertex_dwords = OutDwords;
    so_rt.gs_so_buffers = 1;
    so_rt.props.num_user_data = 4;
    so_rt.props.num_allocated_vgprs = 64;

    const auto es_spv = Compile(EsProgram(), es);
    const auto gs_spv = Compile(GsProgram(k), gs);
    const auto copy_spv = Compile(copy_code, copy, copy_ud);
    const auto so_spv = so ? Compile(copy_code, so_rt, copy_ud) : Compiled{};

    // Scratch, laid out as RecordGsEmulation does.
    const u64 slots = u64(prims) * k.instances;
    const bool points = GsEmu::PointOutput(k.out_prim);
    const u64 esgs_base = k.indirect || points ? GsEmu::IndirectDwords : GsEmu::HeaderDwords;
    const u64 gsvs_base = esgs_base + u64(k.records) * k.instances * EsDwords;
    const u64 index_base = gsvs_base + slots * n * OutDwords;
    const u64 compact_base = index_base + slots * run_length;
    const u64 needed_bytes = (compact_base + (points ? slots * run_length : 0)) * 4;
    const u64 bytes = k.overflow ? needed_bytes - 4 : needed_bytes;
    Buffer scratch = MakeBuffer(bytes);
    std::memset(scratch.data, 0xCD, bytes); // unwritten records stay recognisable
    u32* header = reinterpret_cast<u32*>(scratch.data);
    std::memset(header, 0, GsEmu::HeaderDwords * 4);
    header[GsEmu::RecordsPerInstance] = k.records;
    header[GsEmu::PrimsPerInstance] = prims;
    header[GsEmu::Instances] = k.instances;
    header[GsEmu::BaseVertex] = BaseVertex + (g_control == "base-vertex" ? 1 : 0);
    header[GsEmu::EsgsBase] = u32(esgs_base);
    header[GsEmu::GsvsBase] = u32(gsvs_base);
    header[GsEmu::IndexBase] = u32(index_base);
    header[GsEmu::CompactBase] = u32(compact_base);
    if (points) {
        std::memset(header + GsEmu::CompactDraw, 0, sizeof(VkDrawIndexedIndirectCommand));
    }
    // Guest index buffer: data starts index_skip_bytes into the bound range, which is rounded up to
    // whole dwords and padded with a canary dword.
    const u32 index_size = k.index == Index::U16 ? 2 : 4;
    const u32 gap = k.indirect && k.index != Index::None ? IndirectFirst : 0;
    const u64 index_bytes = u64(k.index_skip_bytes) + u64(gap + k.records) * index_size;
    Buffer index_buffer = MakeBuffer((index_bytes + 3) / 4 * 4 + 4);
    std::memset(index_buffer.data, 0xEE, index_buffer.size);
    for (u32 i = 0; i < k.records; ++i) {
        if (k.index == Index::U16) {
            const u16 v = u16(indices[i]);
            std::memcpy(index_buffer.data + k.index_skip_bytes + (gap + i) * 2, &v, 2);
        } else {
            std::memcpy(index_buffer.data + k.index_skip_bytes + (gap + i) * 4, &indices[i], 4);
        }
    }
    header[GsEmu::IndexKind] = u32(k.index == Index::None  ? GsEmu::IndexKind::None
                                   : k.index == Index::U16 ? GsEmu::IndexKind::U16
                                                           : GsEmu::IndexKind::U32);
    header[GsEmu::FirstIndex] = k.index_skip_bytes / index_size;
    Buffer zero = MakeBuffer(4096);
    Buffer so_buffer = MakeBuffer(u64(so_capacity_dw) * 4);
    std::fill_n(reinterpret_cast<u32*>(so_buffer.data), so_capacity_dw, SoCanary);
    Buffer so_state = MakeBuffer(32); // vk_gs_emulation.h SoState
    std::memset(so_state.data, 0, 32);
    reinterpret_cast<u32*>(so_state.data)[0] = k.so_start; // STRMOUT_BUFFER_UPDATE
    std::vector<Buffer> flats;

    const auto ranges = [&](const Compiled& c) {
        std::vector<VkDescriptorBufferInfo> r;
        for (const auto type : c.buffers) {
            switch (type) {
            case Shader::BufferType::GsScratch:
                r.push_back({scratch.buffer, 0, bytes});
                break;
            case Shader::BufferType::GsIndexInput:
                r.push_back(k.index == Index::None
                                ? VkDescriptorBufferInfo{scratch.buffer, 0, bytes}
                                : VkDescriptorBufferInfo{index_buffer.buffer, 0,
                                                         (index_bytes + 3) / 4 * 4});
                break;
            case Shader::BufferType::Flatbuf:
                if (!c.flat.empty()) {
                    flats.push_back(MakeBuffer(c.flat.size() * 4));
                    std::memcpy(flats.back().data, c.flat.data(), c.flat.size() * 4);
                    r.push_back({flats.back().buffer, 0, flats.back().size});
                    break;
                }
                r.push_back({zero.buffer, 0, zero.size});
                break;
            case Shader::BufferType::ClipPlanes:
                r.push_back({zero.buffer, 0, zero.size});
                break;
            case Shader::BufferType::Guest:
                REQUIRE(&c == &so_spv); // only the stream-out kernel binds a guest buffer
                r.push_back({so_buffer.buffer, 0, k.so_tight ? u64(so_size_dw) * 4 : so_buffer.size});
                break;
            default:
                std::fprintf(stderr, "unexpected buffer type %u\n", u32(type));
                std::exit(2);
            }
        }
        return r;
    };
    const auto es_layout = MakeLayout(VK_SHADER_STAGE_COMPUTE_BIT, ranges(es_spv));
    const auto gs_layout = MakeLayout(VK_SHADER_STAGE_COMPUTE_BIT, ranges(gs_spv));
    const auto draw_layout =
        MakeLayout(VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, ranges(copy_spv));
    Layout so_layout{};
    VkPipeline so_pipe = VK_NULL_HANDLE;
    if (so) {
        so_layout = MakeLayout(VK_SHADER_STAGE_COMPUTE_BIT, ranges(so_spv));
        so_pipe = ComputePipeline(Module(so_spv.spv), so_layout.layout);
    }
    const auto ref_layout = MakeLayout(VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, {});
    const VkShaderModule es_module = Module(es_spv.spv), gs_module = Module(gs_spv.spv),
                         copy_module = Module(copy_spv.spv),
                         fs_module = Module(ReadFile(spv_dir + "/flat.frag.spv")),
                         ref_vs_module = Module(ReadFile(spv_dir + "/list.vert.spv"));
    const VkPipeline es_pipe = ComputePipeline(es_module, es_layout.layout);
    const VkPipeline gs_pipe = ComputePipeline(gs_module, gs_layout.layout);
    const bool tris = k.out_prim == AmdGpu::GsOutputPrimitiveType::TriangleStrip;
    const VkPipeline draw_pipe = GraphicsPipeline(
        copy_module, fs_module, draw_layout.layout,
        points ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST
        : tris ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP
               : VK_PRIMITIVE_TOPOLOGY_LINE_STRIP,
        !points, false);
    const VkPipeline ref_pipe = GraphicsPipeline(ref_vs_module, fs_module, ref_layout.layout,
                                                 points ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST
                                                 : tris ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
                                                        : VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
                                                 false, true);
    // Point output: the production compaction kernel.
    VkPipeline compact_pipe = VK_NULL_HANDLE;
    Layout compact_layout{};
    if (points) {
        // Binding 1: the stream-out state (any buffer without stream-out), as recorded.
        compact_layout = MakeLayout(VK_SHADER_STAGE_COMPUTE_BIT,
                                    {{scratch.buffer, 0, bytes},
                                     so ? VkDescriptorBufferInfo{so_state.buffer, 0, 32}
                                        : VkDescriptorBufferInfo{scratch.buffer, 0, bytes}});
        compact_pipe = ComputePipeline(Module(ReadFile(spv_dir + "/gs_point_compact.comp.spv")),
                                       compact_layout.layout);
    }

    Expected want = k.overflow ? Expected{} : Model(k, vertex_ids);
    Buffer vertices = MakeBuffer(std::max<u64>(want.list.size(), 1) * sizeof(Vertex));
    std::memcpy(vertices.data, want.list.data(), want.list.size() * sizeof(Vertex));
    // Indirect: the guest command, the error words and the production setup kernel.
    Buffer args = MakeBuffer(64), errors = MakeBuffer(32);
    VkPipeline setup_pipe = VK_NULL_HANDLE;
    Layout setup_layout{};
    SetupParams setup{};
    if (k.indirect) {
        const u32 base_vertex = BaseVertex + (g_control == "base-vertex" ? 1 : 0);
        const u32 command[5] = {k.records, k.instances,
                                k.index != Index::None ? IndirectFirst : base_vertex,
                                k.index != Index::None ? base_vertex : 0u, 0u};
        std::memcpy(args.data + 4, command, sizeof(command)); // one dword in: args_skip
        setup = {.args_skip = 1,
                 .indexed = k.index != Index::None,
                 .index_kind = u32(k.index == Index::U16 ? GsEmu::IndexKind::U16
                                                         : GsEmu::IndexKind::U32),
                 .index_skip = k.index_skip_bytes / index_size,
                 .index_count = gap + k.records,
                 .verts_per_prim = per,
                 .invocations = 1,
                 .max_vert_out = n,
                 .esgs_dwords = EsDwords,
                 .vertex_dwords = OutDwords,
                 .run_length = run_length,
                 .capacity_dwords = u32(bytes / 4),
                 .args_address_lo = 0x1234560,
                 .args_address_hi = 0x7f,
                 .point_output = points ? 1u : 0u};
        // The CPU header is not used: the kernel writes it.
        std::memset(scratch.data, 0xCD, GsEmu::IndirectDwords * 4);
        std::vector<VkDescriptorBufferInfo> r = {{args.buffer, 0, 64},
                                                 {scratch.buffer, 0, bytes},
                                                 {errors.buffer, 0, 32}};
        setup_layout = MakeLayout(VK_SHADER_STAGE_COMPUTE_BIT, r);
        // MakeLayout's push range is PushData-sized, which covers SetupParams.
        setup_pipe = ComputePipeline(Module(ReadFile(spv_dir + "/gs_indirect_setup.comp.spv")),
                                     setup_layout.layout);
    }
    const Target target = MakeTarget();
    Buffer image = MakeBuffer(u64(Size) * Size * 16), ref_image = MakeBuffer(u64(Size) * Size * 16);
    const Shader::PushData push{};

    // Emulated draw.
    Begin();
    if (k.indirect) {
        vkCmdBindPipeline(g.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, setup_pipe);
        vkCmdBindDescriptorSets(g.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, setup_layout.layout, 0, 1,
                                &setup_layout.set, 0, nullptr);
        vkCmdPushConstants(g.cmd, setup_layout.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(setup), &setup);
        vkCmdDispatch(g.cmd, 1, 1, 1);
        Barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    }
    vkCmdBindPipeline(g.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, es_pipe);
    vkCmdBindDescriptorSets(g.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, es_layout.layout, 0, 1,
                            &es_layout.set, 0, nullptr);
    vkCmdPushConstants(g.cmd, es_layout.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    if (k.indirect) {
        vkCmdDispatchIndirect(g.cmd, scratch.buffer, GsEmu::EsDispatch * 4);
    } else {
        vkCmdDispatch(g.cmd, (k.records + GsEmu::WorkgroupSize - 1) / GsEmu::WorkgroupSize,
                      k.instances, 1);
    }
    Barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    const auto gs_pass = [&] {
        vkCmdBindPipeline(g.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, gs_pipe);
        vkCmdBindDescriptorSets(g.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, gs_layout.layout, 0, 1,
                                &gs_layout.set, 0, nullptr);
        vkCmdPushConstants(g.cmd, gs_layout.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push),
                           &push);
        if (k.indirect) {
            vkCmdDispatchIndirect(g.cmd, scratch.buffer, GsEmu::GsDispatch * 4);
        } else if (prims) {
            vkCmdDispatch(g.cmd, (prims + GsEmu::WorkgroupSize - 1) / GsEmu::WorkgroupSize,
                          k.instances, 1);
        }
        if (!points) {
            return;
        }
        Barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        // vk_gs_emulation.cpp CompactParams: invocations, run_length, stream-out buffers,
        // VGT_STRMOUT_BUFFER_SIZE_0..3, VGT_STRMOUT_VTX_STRIDE_0..3.
        const u32 compact_params[11] = {1, run_length, so ? 1u : 0u, so_size_dw, 0, 0, 0,
                                        SoStrideDw, 0, 0, 0};
        vkCmdBindPipeline(g.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, compact_pipe);
        vkCmdBindDescriptorSets(g.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, compact_layout.layout, 0, 1,
                                &compact_layout.set, 0, nullptr);
        vkCmdPushConstants(g.cmd, compact_layout.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(compact_params), compact_params);
        vkCmdDispatch(g.cmd, 1, 1, 1);
        if (!so) {
            return;
        }
        // RecordGsStreamOut: the kernel over the compacted points, sized by the tail above.
        Barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
        vkCmdBindPipeline(g.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, so_pipe);
        vkCmdBindDescriptorSets(g.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, so_layout.layout, 0, 1,
                                &so_layout.set, 0, nullptr);
        vkCmdPushConstants(g.cmd, so_layout.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push),
                           &push);
        vkCmdDispatchIndirect(g.cmd, scratch.buffer, GsEmu::SoDispatch * 4);
    };
    gs_pass();
    for (u32 d = 1; so && d < k.so_draws; ++d) {
        // The same draw again: GS, compaction and stream-out after the previous ones.
        Barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                    VK_ACCESS_2_TRANSFER_WRITE_BIT);
        if (k.so_reset) {
            const u32 zero_offset = 0; // Rasterizer::StreamOutUpdate
            vkCmdUpdateBuffer(g.cmd, so_state.buffer, 0, 4, &zero_offset);
            Barrier(VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        }
        gs_pass();
    }
    Barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT |
                VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT |
                VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
    Render(target, image, [&] {
        vkCmdBindPipeline(g.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, draw_pipe);
        vkCmdBindDescriptorSets(g.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, draw_layout.layout, 0, 1,
                                &draw_layout.set, 0, nullptr);
        vkCmdPushConstants(g.cmd, draw_layout.layout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(push), &push);
        if (k.indirect || points) {
            vkCmdBindIndexBuffer(g.cmd, scratch.buffer, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexedIndirect(g.cmd, scratch.buffer,
                                     (points ? GsEmu::CompactDraw : GsEmu::StripDraw) * 4, 1,
                                     sizeof(VkDrawIndexedIndirectCommand));
        } else if (slots) {
            vkCmdBindIndexBuffer(g.cmd, scratch.buffer, index_base * 4, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(g.cmd, u32(slots * run_length), 1, 0, 0, 0);
        }
    });
    Barrier(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
            VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    Submit();

    // Reference draw.
    Begin();
    Render(target, ref_image, [&] {
        vkCmdBindPipeline(g.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ref_pipe);
        vkCmdPushConstants(g.cmd, ref_layout.layout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(push), &push);
        const VkDeviceSize zero_offset = 0;
        vkCmdBindVertexBuffers(g.cmd, 0, 1, &vertices.buffer, &zero_offset);
        if (!want.list.empty()) {
            vkCmdDraw(g.cmd, u32(want.list.size()), 1, 0, 0);
        }
    });
    Barrier(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
            VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    Submit();

    // Compare.
    std::string failures;
    if (k.indirect) {
        const u32* e = reinterpret_cast<const u32*>(errors.data);
        const bool want_report = k.overflow;
        if ((e[0] != 0) != want_report ||
            (want_report && (e[1] != needed_bytes / 4 || e[3] != 0x1234560 || e[4] != 0x7f))) {
            char line[200];
            std::snprintf(line, sizeof line,
                          "    setup kernel reports %u (largest need %u dwords, %u index overruns); "
                          "want %s %llu\n",
                          e[0], e[1], e[2], want_report ? "a report of" : "none, need",
                          (unsigned long long)(needed_bytes / 4));
            failures += line;
        }
        if (k.overflow) {
            // Nothing else is meaningful: the draw must be empty.
            want.runs.clear();
        }
    }
    const u32* runs = reinterpret_cast<const u32*>(scratch.data) + index_base;
    u32 run_diffs = 0;
    for (size_t i = 0; i < want.runs.size() && !k.overflow; ++i) {
        if (runs[i] != want.runs[i] && run_diffs++ < 8) {
            char line[160];
            std::snprintf(line, sizeof line, "    index run entry %zu (slot %zu): got %#x want %#x\n",
                          i, i / run_length, runs[i], want.runs[i]);
            failures += line;
        }
    }
    if (points && !k.overflow) {
        const u32* draw = reinterpret_cast<const u32*>(scratch.data) + GsEmu::CompactDraw;
        const u32* compact = reinterpret_cast<const u32*>(scratch.data) + compact_base;
        u32 diffs = 0;
        if (draw[0] != want.compact.size() || draw[1] != 1 || draw[2] != compact_base) {
            char line[200];
            std::snprintf(line, sizeof line,
                          "    point draw: %u indices from %u (x%u), want %zu from %llu\n",
                          draw[0], draw[2], draw[1], want.compact.size(),
                          (unsigned long long)compact_base);
            failures += line;
        }
        for (size_t i = 0; i < want.compact.size(); ++i) {
            if (compact[i] != want.compact[i] && diffs++ < 8) {
                char line[160];
                std::snprintf(line, sizeof line, "    compacted index %zu: got %#x want %#x\n", i,
                              compact[i], want.compact[i]);
                failures += line;
            }
        }
    }
    u32 so_diffs = 0, so_written = 0;
    if (so) {
        // CPU model: per draw, the leading vertices (output order) that fit in buffer 0 from its
        // offset, vertex i at offset + i * stride; the offset advances, overflow is counted.
        std::vector<const Vertex*> by_record(slots * n, nullptr);
        for (const auto& [record, v] : want.records) {
            by_record[record] = &v;
        }
        std::vector<u32> want_so(so_capacity_dw, SoCanary);
        const u32 total = u32(want.compact.size());
        u32 offset = k.so_start, start = 0, count = 0, overflow_draws = 0, dropped = 0;
        for (u32 d = 0; d < k.so_draws; ++d) {
            offset = d > 0 && k.so_reset ? 0 : offset;
            const u32 fit = so_size_dw > offset ? (so_size_dw - offset) / SoStrideDw : 0;
            count = std::min(total, fit);
            for (u32 i = 0; i < count; ++i) {
                // Negative control: neighbouring lanes swapped.
                const u32 at = offset + (g_control == "so-lane" ? i ^ 1 : i) * SoStrideDw;
                for (u32 c = 0; c < 4 && at + c < so_capacity_dw; ++c) {
                    std::memcpy(&want_so[at + c], &by_record[want.compact[i]]->data[4 + c], 4);
                }
            }
            if (count < total) {
                ++overflow_draws;
                dropped += total - count;
            }
            start = offset;
            offset += count * SoStrideDw;
        }
        so_written = count;
        const u32* got_so = reinterpret_cast<const u32*>(so_buffer.data);
        for (u32 i = 0; i < so_capacity_dw; ++i) {
            if (got_so[i] != want_so[i] && so_diffs++ < 8) {
                char line[160];
                std::snprintf(line, sizeof line,
                              "    stream-out dword %u (vertex %u): got %#x want %#x\n", i,
                              (i - std::min(i, k.so_start)) / SoStrideDw, got_so[i], want_so[i]);
                failures += line;
            }
        }
        const u32* state = reinterpret_cast<const u32*>(so_state.data);
        const u32* sx = reinterpret_cast<const u32*>(scratch.data);
        if (state[0] != offset || state[4] != overflow_draws || state[5] != dropped ||
            sx[GsEmu::SoCount] != count || sx[GsEmu::SoOffset0] != start ||
            sx[GsEmu::SoDispatch] != (count + 63) / 64) {
            char line[300];
            std::snprintf(line, sizeof line,
                          "    stream-out state: offset %u, %u overflowed draws, %u dropped; last "
                          "draw count %u from %u, %u groups; want %u, %u, %u; %u from %u\n",
                          state[0], state[4], state[5], sx[GsEmu::SoCount], sx[GsEmu::SoOffset0],
                          sx[GsEmu::SoDispatch], offset, overflow_draws, dropped, count, start);
            failures += line;
            ++so_diffs;
        }
    }
    u32 record_diffs = 0;
    const float* gsvs = reinterpret_cast<const float*>(scratch.data) + gsvs_base;
    for (const auto& [record, v] : want.records) {
        if (std::memcmp(gsvs + u64(record) * OutDwords, v.data.data(), sizeof(v.data)) != 0 &&
            record_diffs++ < 8) {
            const float* got = gsvs + u64(record) * OutDwords;
            char line[256];
            std::snprintf(line, sizeof line,
                          "    GSVS record %u: got (%g %g %g %g | %g %g %g %g) want (%g %g %g %g | "
                          "%g %g %g %g)\n",
                          record, got[0], got[1], got[2], got[3], got[4], got[5], got[6], got[7],
                          v.data[0], v.data[1], v.data[2], v.data[3], v.data[4], v.data[5],
                          v.data[6], v.data[7]);
            failures += line;
        }
    }
    u32 esgs_diffs = 0;
    const float* esgs = reinterpret_cast<const float*>(scratch.data) + esgs_base;
    for (u32 inst = 0; inst < (k.overflow ? 0 : k.instances); ++inst) {
        for (u32 i = 0; i < k.records; ++i) {
            const float* got = esgs + (u64(inst) * k.records + i) * EsDwords;
            const float want_rec[EsDwords] = {float(vertex_ids[i]), float(inst), 0.0f, 1.0f};
            if (std::memcmp(got, want_rec, sizeof(want_rec)) != 0 && esgs_diffs++ < 4) {
                char line[200];
                std::snprintf(line, sizeof line,
                              "    ESGS record %u (instance %u): got (%g %g %g %g) want (%g %g 0 1)\n",
                              i, inst, got[0], got[1], got[2], got[3], want_rec[0], want_rec[1]);
                failures += line;
            }
        }
    }
    u32 pixel_diffs = 0, covered = 0;
    const float* a = reinterpret_cast<const float*>(image.data);
    const float* b = reinterpret_cast<const float*>(ref_image.data);
    for (u32 i = 0; i < Size * Size; ++i) {
        covered += (b[i * 4] != 0 || b[i * 4 + 1] != 0 || b[i * 4 + 3] != 0) ? 1 : 0;
        if (std::memcmp(a + i * 4, b + i * 4, 16) != 0 && pixel_diffs++ < 4) {
            char line[200];
            std::snprintf(line, sizeof line,
                          "    pixel (%u,%u): got (%g %g %g %g) want (%g %g %g %g)\n", i % Size,
                          i / Size, a[i * 4], a[i * 4 + 1], a[i * 4 + 2], a[i * 4 + 3], b[i * 4],
                          b[i * 4 + 1], b[i * 4 + 2], b[i * 4 + 3]);
            failures += line;
        }
    }
    const bool pass = failures.empty();
    std::printf("%-58s %s  (%u slots, %zu emitted, %zu list vertices, %u px covered", k.name,
                pass ? "PASS" : "FAIL", u32(slots), want.records.size(), want.list.size(), covered);
    if (so) {
        std::printf(", %u streamed out", so_written);
    }
    std::printf(")\n");
    if (!pass) {
        std::printf("    %u index-run, %u GSVS record, %u ESGS record, %u pixel, %u stream-out "
                    "differences\n%s",
                    run_diffs, record_diffs, esgs_diffs, pixel_diffs, so_diffs, failures.c_str());
    }
    for (Buffer* buf : {&scratch, &index_buffer, &zero, &vertices, &image, &ref_image, &args,
                        &errors, &so_buffer, &so_state}) {
        FreeBuffer(*buf);
    }
    for (Buffer& buf : flats) {
        FreeBuffer(buf);
    }
    return pass;
}

} // namespace

/// Which vertex a flat varying takes per triangle on this device, against Vulkan's FIRST
/// provoking vertex, which is also per-vertex index 0 of VK_KHR_fragment_shader_barycentric:
/// list triangle i -> vertex 3i, strip triangle i -> vertex i (odd triangles too), fan triangle
/// i -> vertex i + 1. Every triangle covers its own pixels; vertex v's flat value is v + 1 (the
/// vertex buffer index the index buffer names), read at the triangle's centroid. Indexed draws;
/// strips restart between segments, like the emulated GS's strip draws.
bool RunProvokingCase(const char* name, VkPrimitiveTopology topology,
                      const std::vector<u32>& segment_lengths, const std::string& spv_dir) {
    const bool strip = topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    const bool fan = topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    struct Tri {
        std::array<u32, 3> slots; // positions in the index buffer, Vulkan's vertex order
        u32 provoking;            // index buffer position of the expected vertex
    };
    std::vector<std::array<float, 2>> slot_pos; // per index-buffer position (restarts too)
    std::vector<Tri> tris;
    const u32 rows = u32(segment_lengths.size());
    std::vector<u32> slot_of_index; // index-buffer position -> slot, or ~0 for a restart
    for (u32 row = 0; row < rows; ++row) {
        const u32 n = segment_lengths[row];
        const float top = -1.0f + 2.0f * row / rows + 0.04f, bottom = top + 2.0f / rows - 0.08f;
        const u32 first = u32(slot_pos.size());
        for (u32 k = 0; k < n; ++k) {
            float x, y;
            if (fan) {
                // Center first, then a rim of increasing angle: disjoint sectors.
                const float cx = 0.0f, cy = (top + bottom) / 2, r = (bottom - top) / 2;
                const float a = 3.1f * float(k - 1) / float(n - 2) + 0.02f;
                x = k == 0 ? cx : cx + 5.0f * r * std::cos(a);
                y = k == 0 ? cy : cy - r * std::sin(a) * 0.95f;
            } else if (strip) {
                x = -0.95f + 1.9f * k / (n - 1);
                y = (k % 2) ? bottom : top;
            } else {
                const u32 t = k / 3, c = k % 3, tris_in_row = n / 3;
                const float w = 1.9f / tris_in_row, x0 = -0.95f + w * t;
                x = c == 0 ? x0 + 0.1f * w : c == 1 ? x0 + 0.9f * w : x0 + 0.5f * w;
                y = c == 2 ? bottom : top;
            }
            slot_pos.push_back({x, y});
        }
        for (u32 j = 0; j + 2 < n; j += strip || fan ? 1 : 3) {
            const u32 a = first + j;
            if (strip) {
                // Vulkan strip order: (i, i + 1 + i % 2, i + 2 - i % 2); provoking i.
                tris.push_back({{a, a + 1 + j % 2, a + 2 - j % 2}, a});
            } else if (fan) {
                // Vulkan fan order: (i + 1, i + 2, 0); provoking i + 1.
                tris.push_back({{a + 1, a + 2, first}, a + 1});
            } else {
                tris.push_back({{a, a + 1, a + 2}, a});
            }
        }
    }
    // Index buffer: slot k names vertex perm[k] (a fixed shuffle), restarts between segments.
    const u32 slots = u32(slot_pos.size());
    std::vector<u32> perm(slots);
    for (u32 k = 0; k < slots; ++k) {
        perm[k] = (k * 7 + 3) % slots;
    }
    if (std::gcd(7u, slots) != 1) {
        for (u32 k = 0; k < slots; ++k) perm[k] = slots - 1 - k;
    }
    std::vector<u32> indices;
    u32 slot = 0;
    for (u32 row = 0; row < rows; ++row) {
        if (row > 0 && (strip || fan)) {
            indices.push_back(0xFFFFFFFFu);
        }
        for (u32 k = 0; k < segment_lengths[row]; ++k) {
            indices.push_back(perm[slot++]);
        }
    }
    Buffer vertices = MakeBuffer(slots * sizeof(Vertex));
    for (u32 k = 0; k < slots; ++k) {
        Vertex v{};
        v.data = {slot_pos[k][0], slot_pos[k][1], 0.0f, 1.0f, float(perm[k] + 1), 0.0f, 0.0f, 1.0f};
        std::memcpy(vertices.data + perm[k] * sizeof(Vertex), &v, sizeof(v));
    }
    Buffer index_buffer = MakeBuffer(indices.size() * 4);
    std::memcpy(index_buffer.data, indices.data(), indices.size() * 4);
    Buffer image = MakeBuffer(u64(Size) * Size * 16);
    const Target target = MakeTarget();
    const auto layout = MakeLayout(VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, {});
    const VkPipeline pipe = GraphicsPipeline(Module(ReadFile(spv_dir + "/list.vert.spv")),
                                             Module(ReadFile(spv_dir + "/flat.frag.spv")),
                                             layout.layout, topology, strip || fan, true);
    Begin();
    Render(target, image, [&] {
        vkCmdBindPipeline(g.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        const VkDeviceSize zero = 0;
        vkCmdBindVertexBuffers(g.cmd, 0, 1, &vertices.buffer, &zero);
        vkCmdBindIndexBuffer(g.cmd, index_buffer.buffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(g.cmd, u32(indices.size()), 1, 0, 0, 0);
    });
    Barrier(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
            VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    Submit();
    // The pipeline blends dst = 0.5 * src + 0.5 * dst over a cleared target: a pixel holds half
    // the flat value.
    const float* px = reinterpret_cast<const float*>(image.data);
    u32 wrong = 0, last_vertex = 0, other = 0;
    std::string failures;
    for (size_t t = 0; t < tris.size(); ++t) {
        const auto& tri = tris[t];
        float cx = 0, cy = 0;
        for (const u32 k : tri.slots) {
            cx += slot_pos[k][0] / 3;
            cy += slot_pos[k][1] / 3;
        }
        const u32 x = u32((cx * 0.5f + 0.5f) * Size), y = u32((cy * 0.5f + 0.5f) * Size);
        const float got = px[(y * Size + x) * 4] * 2.0f;
        // Control "provoking-last": expect the triangle's last vertex instead (must fail).
        const u32 want_slot = g_control == "provoking-last" ? tri.slots[2] : tri.provoking;
        const float want = float(perm[want_slot] + 1);
        if (got != want) {
            ++wrong;
            last_vertex += got == float(perm[tri.slots[2]] + 1) ? 1 : 0;
            other += got != float(perm[tri.slots[2]] + 1) ? 1 : 0;
            if (wrong <= 6) {
                char line[200];
                std::snprintf(line, sizeof line,
                              "    triangle %zu (slots %u %u %u): flat value from vertex %g, want "
                              "%g (Vulkan FIRST)\n",
                              t, tri.slots[0], tri.slots[1], tri.slots[2], got - 1, want - 1);
                failures += line;
            }
        }
    }
    std::printf("%-58s %s  (%zu triangles; %u wrong: %u take Vulkan's vertex 2, %u other)\n", name,
                wrong ? "FAIL" : "PASS", tris.size(), wrong, last_vertex, other);
    std::fputs(failures.c_str(), stdout);
    return wrong == 0;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: conformance SPV_DIR [--only SUBSTRING | --provoking]\n");
        return 2;
    }
    const std::string spv_dir = argv[1];
    std::string only;
    bool provoking = false;
    for (int i = 2; i < argc; ++i) {
        provoking |= std::string(argv[i]) == "--provoking";
    }
    for (int i = 2 + (provoking && argc > 2 && std::string(argv[2]) == "--provoking");
         i + 1 < argc; i += 2) {
        if (std::string(argv[i]) == "--only") {
            only = argv[i + 1];
        } else if (std::string(argv[i]) == "--control") {
            g_control = argv[i + 1];
        }
    }
    using P = AmdGpu::PrimitiveType;
    using O = AmdGpu::GsOutputPrimitiveType;
    std::vector<Case> cases = {
        {"points -> tri strips, N=8 (config-1 shape), 64 points", P::PointList, O::TriangleStrip, 8, 64, 1, Index::None, 0},
        {"points -> tri strips, N=6: emits past N dropped", P::PointList, O::TriangleStrip, 6, 64, 1, Index::None, 0},
        {"points -> tri strips, N=3", P::PointList, O::TriangleStrip, 3, 48, 1, Index::None, 0},
        {"points -> tri strips, N=1: nothing draws, all padding", P::PointList, O::TriangleStrip, 1, 32, 1, Index::None, 0},
        {"points -> tri strips, N=8, 3 instances", P::PointList, O::TriangleStrip, 8, 40, 3, Index::None, 0},
        {"points, u16 indices, odd count, 2-byte skip, 2 instances", P::PointList, O::TriangleStrip, 8, 37, 2, Index::U16, 2},
        {"points, u16 indices, count 1", P::PointList, O::TriangleStrip, 8, 1, 1, Index::U16, 0},
        {"points, u16 indices, count 1, 2-byte skip", P::PointList, O::TriangleStrip, 8, 1, 1, Index::U16, 2},
        {"points, u32 indices, 8-byte skip", P::PointList, O::TriangleStrip, 8, 50, 1, Index::U32, 8},
        {"triangle list in, N=4, u16", P::TriangleList, O::TriangleStrip, 4, 61, 1, Index::U16, 0},
        {"line list in, line strips out, N=5", P::LineList, O::LineStrip, 5, 41, 2, Index::None, 0},
        {"points -> line strips, N=8", P::PointList, O::LineStrip, 8, 64, 1, Index::None, 0},
        {"points, 5000: many workgroups, partial last group", P::PointList, O::TriangleStrip, 8, 5000, 1, Index::None, 0},
        {"points, N=32", P::PointList, O::TriangleStrip, 32, 256, 1, Index::None, 0},
        {"indirect: points, N=8, 3 instances", P::PointList, O::TriangleStrip, 8, 64, 3, Index::None, 0, true},
        {"indirect: u16, firstIndex 3, 2-byte skip, 2 instances", P::PointList, O::TriangleStrip, 8, 37, 2, Index::U16, 2, true},
        {"indirect: triangle list in, u32, N=4", P::TriangleList, O::TriangleStrip, 4, 60, 1, Index::U32, 0, true},
        {"indirect: scratch one dword short -> empty + report", P::PointList, O::TriangleStrip, 8, 64, 1, Index::None, 0, true, true},
        {"points -> points, N=4", P::PointList, O::PointList, 4, 64, 1, Index::None, 0},
        {"triangle list in -> points, N=8, u16, 2 instances", P::TriangleList, O::PointList, 8, 61, 2, Index::U16, 0},
        {"points -> points, 5000: compaction over many chunks", P::PointList, O::PointList, 2, 5000, 1, Index::None, 0},
        {"indirect: points -> points, N=8, 3 instances", P::PointList, O::PointList, 8, 64, 3, Index::None, 0, true},
    };
    // Stream-out: 64 points in, p & 7 points out each (224), so the count is the room until 224.
    const auto so_case = [&](const char* name, SoStore store, u32 room, u32 start = 0,
                             u32 draws = 1, bool reset = false, bool indirect = false,
                             bool tight = false) {
        Case k{name, P::PointList, O::PointList, 8, 64, 1, Index::None, 0, indirect};
        k.so_store = store;
        k.so_room = room;
        k.so_start = start;
        k.so_draws = draws;
        k.so_reset = reset;
        k.so_tight = tight;
        cases.push_back(k);
    };
    so_case("stream-out raw: room 1 (overflow)", SoStore::Raw, 1);
    so_case("stream-out raw: room 31", SoStore::Raw, 31);
    so_case("stream-out raw: room 32", SoStore::Raw, 32);
    so_case("stream-out raw: room 33 (second subgroup)", SoStore::Raw, 33);
    so_case("stream-out raw: room 63", SoStore::Raw, 63);
    so_case("stream-out raw: room 64 (one full wave)", SoStore::Raw, 64);
    so_case("stream-out raw: room 65 (second wave)", SoStore::Raw, 65);
    so_case("stream-out raw: all 224 fit, offset 6", SoStore::Raw, 300, 6);
    so_case("stream-out ballot of exec: room 33", SoStore::RawBallot, 33);
    so_case("stream-out ballot of exec: room 63", SoStore::RawBallot, 63);
    so_case("stream-out typed xyzw: room 33", SoStore::Typed, 33);
    so_case("stream-out typed xyzw: room 65", SoStore::Typed, 65);
    so_case("stream-out typed xyzw: all 224 fit, offset 5", SoStore::Typed, 1000, 5);
    so_case("stream-out: 2 draws append, second overflows", SoStore::Raw, 300, 0, 2);
    so_case("stream-out: offset reset to 0 between 2 draws", SoStore::Typed, 300, 0, 2, true);
    so_case("indirect: stream-out typed, room 100", SoStore::Typed, 100, 3, 1, false, true);
    so_case("stream-out: offset 8, last record at the V# end", SoStore::Typed, 50, 8, 1, false,
            false, true);
    InitGpu();
    if (provoking) {
        const auto list = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        const auto strip = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        const auto fan = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
        VkPhysicalDevicePortabilitySubsetFeaturesKHR portability{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PORTABILITY_SUBSET_FEATURES_KHR};
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &portability};
        vkGetPhysicalDeviceFeatures2(g.physical, &features);
        int failed = 0, ran = 0;
        const auto run_case = [&](const char* name, VkPrimitiveTopology t, std::vector<u32> rows) {
            ++ran;
            failed += RunProvokingCase(name, t, rows, spv_dir) ? 0 : 1;
        };
        run_case("provoking: triangle list, 4 rows x 5 triangles", list, {15, 15, 15, 15});
        run_case("provoking: strips 3/4/5/8/9 vertices, restarts", strip, {3, 4, 5, 8, 9});
        run_case("provoking: strips 12 vertices x 4, restarts", strip, {12, 12, 12, 12});
        if (portability.triangleFans) {
            run_case("provoking: fans 5/6/8 vertices, restarts", fan, {5, 6, 8});
        } else {
            std::printf("provoking: fans: the device has no triangle fans (portability subset)\n");
        }
        std::printf("provoking vertex: %d of %d cases passed\n", ran - failed, ran);
        return failed ? 1 : 0;
    }
    int failed = 0, ran = 0;
    for (const auto& k : cases) {
        if (!only.empty() && std::string(k.name).find(only) == std::string::npos) {
            continue;
        }
        ++ran;
        failed += RunCase(k, spv_dir) ? 0 : 1;
    }
    std::printf("%d of %d cases passed\n", ran - failed, ran);
    return failed ? 1 : 0;
}
