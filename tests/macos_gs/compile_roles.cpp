// SPDX-License-Identifier: GPL-2.0-or-later
// Geometry emulation (Apple): compiles a dumped ES/GS/copy-shader triplet (the gs_report files the
// GS abort writes) through the production recompiler in the three emulation roles (EsCompute,
// GsCompute, CopyVertex), writes the SPIR-V, and lists each role's buffers. spirv-val runs from
// build.sh. The configuration comes from the "Geometry configuration:" log line.
//
//   compile_roles DIR ES_HASH GS_HASH OUT_DIR prim=N out_prim=N max_vert_out=N esgs=N vert=N
//                 [instances=N]
//   compile_roles --negative DIR|- GS_HASH OUT_DIR
//       compile-negative controls: hand-encoded ES/GS/copy programs using what the emulation roles
//       cannot run must stop loudly (each in a child process), naming the feature.
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/tessellation.h"
#include "shader_recompiler/gs_emulation.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/runtime_info.h"
#include "tests/macos_gs/gs_asm.h"
#include "video_core/amdgpu/regs_shader.h"

namespace {

std::vector<u32> ReadWords(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "compile_roles: cannot read %s\n", path.c_str());
        std::exit(2);
    }
    std::vector<char> bytes((std::istreambuf_iterator<char>(f)), {});
    std::vector<u32> words(bytes.size() / 4);
    std::memcpy(words.data(), bytes.data(), words.size() * 4);
    return words;
}

/// Guest pointers in the dumped user data are mapped as zeroed memory, so the recompiler's
/// constant reads through them (SRT walks) see a defined, empty table.
void MapUserDataPointers(const std::array<u32, 16>& ud) {
    for (u32 i = 0; i + 1 < ud.size(); i += 2) {
        const u64 address = u64(ud[i]) | (u64(ud[i + 1] & 0xFFFF) << 32);
        if (address < (1ull << 32) || address >= (1ull << 40)) {
            continue;
        }
        const u64 page = address & ~u64(0xFFFF);
        // Fixed allocation that fails instead of replacing an existing mapping: words that only
        // look like a pointer (V# fields) may collide; a real pointer that could not be mapped
        // makes the recompiler fault loudly later.
        mach_vm_address_t at = page;
        if (mach_vm_allocate(mach_task_self(), &at, 1 << 20, VM_FLAGS_FIXED) != KERN_SUCCESS) {
            std::fprintf(stderr, "compile_roles: user-data word pair %u (%#llx) not mapped\n", i,
                         (unsigned long long)address);
        }
    }
}

Shader::Profile AppleProfile() {
    Shader::Profile p{};
    // As MoltenVK reports (conformance reads the device): no subgroup operations in vertex
    // functions, so vertex-stage wave operations take the no-subgroup lowering.
    p.subgroup_supported_stages = ~0x1u;
    p.max_viewport_width = 16384;
    p.max_viewport_height = 16384;
    p.max_shared_memory_size = 32768;
    p.supported_spirv = 0x00010600;
    p.subgroup_size = 32;
    p.support_int8 = true;
    p.support_int16 = true;
    p.support_int64 = true;
    p.support_float16 = true;
    // MoltenVK 1.4.2 on the M4 (Opus-Build's vulkaninfo): RTE and signed-zero/Inf/NaN preserve
    // for fp16/fp32, no denorm controls, no fp64.
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

struct Config {
    std::map<std::string, std::string> kv;
    u32 Get(const char* key, u32 fallback = ~0u) const {
        const auto it = kv.find(key);
        if (it == kv.end()) {
            if (fallback == ~0u) {
                std::fprintf(stderr, "compile_roles: missing %s=\n", key);
                std::exit(2);
            }
            return fallback;
        }
        return u32(std::strtoul(it->second.c_str(), nullptr, 0));
    }
};

int Compile(const char* name, const std::vector<u32>& code, const std::array<u32, 16>& ud,
            u64 hash, Shader::RuntimeInfo rt, const std::filesystem::path& out) {
    static Shader::Pools pools;
    const auto profile = AppleProfile();
    const Shader::ShaderParams params{.user_data = std::span<const u32, 16>(ud),
                                      .code = std::span<const u32>(code),
                                      .hash = hash};
    Shader::Info info(rt.hw_stage, rt.sw_stage, params);
    Shader::Backend::Bindings binding{};
    const auto program = Shader::TranslateProgram(code, pools, info, rt, profile);
    const auto spv = Shader::Backend::SPIRV::EmitSPIRV(profile, rt, program, binding);
    const auto path = out / (std::string(name) + ".spv");
    std::ofstream(path, std::ios::binary)
        .write(reinterpret_cast<const char*>(spv.data()), std::streamsize(spv.size() * 4));
    std::printf("%-12s %6zu words -> %s; buffers:", name, spv.size(), path.c_str());
    for (const auto& b : info.buffers) {
        std::printf(" %u%s", u32(b.buffer_type), b.is_written ? "w" : "");
    }
    std::printf("\n");
    return 0;
}

// Sea Islands encodings for the negative controls.
constexpr u32 SBarrier = 0xBF8A0000u;
constexpr u32 SEndpgm = 0xBF810000u;
// ds_read_b32 v1, v0
constexpr u32 DsReadB32[2] = {0xD8D80000u, 1u << 24};
// v_writelane_b32 v1, s0, lane 0 (VOP2 op 2: vsrc1 field = lane select operand, inline 0)
constexpr u32 VWritelane = (2u << 25) | (1u << 17) | (128u << 9) | 0u;
// buffer_store_dword v1, s[0:3], offset, glc slc, soffset 0 (a ring store)
std::array<u32, 2> RingStore(u32 offset) {
    return {(0x38u << 26) | (28u << 18) | (1u << 14) | offset,
            (0u) | (1u << 8) | (0u << 16) | (1u << 22) | (128u << 24)};
}

enum class Role { Es, Gs, Copy, CopyStreamOut, Hull };

struct Negative {
    const char* name;
    Role role;
    std::vector<u32> code;
    const char* expect;
    u32 so_buffers = 0; // copy roles: RuntimeInfo::gs_so_buffers
    std::vector<u32> copy;  // GS role: a copy shader other than the default
    bool so_table = false;  // s[2:3] points at a stream-out table (V# 0: 16-byte vertices)
};

/// A hull shaped like GXFfxtessellateTracer* (2 GCN threads per patch, quad): an LDS write at the
/// HsOutputBase tess constant (so TessellationPreprocess finds the constants V# at s[4:7]), then
/// six tess factor stores of 1.0 at byte offset f(cp) + 4k (offen, soffset = s8, the factor
/// buffer base after 8 user SGPRs).
enum class TracerHull { Tracer, UpperUnaligned, UpperFar, MixedValues, NoCover };
std::vector<u32> TracerHullProgram(TracerHull kind) {
    using namespace GsAsm;
    Code c;
    c.SMovLiteral(VCC_HI, 0);
    c.SBufferLoadDword(16, 4, 3); // s16 = HsOutputBase
    c.VMov(20, 16);
    c.VMov(4, InlineF1);
    c.VMovLiteral(5, FloatBits(0.5f));
    c.DsWriteB32(20, 4);
    c.VBfeU32(0, Vgpr(1), Inline(8), Inline(5)); // control point id
    switch (kind) {
    case TracerHull::UpperUnaligned:
        c.VMov(3, Vgpr(0)); // cp * 1
        break;
    case TracerHull::UpperFar:
        c.VLshlrevB32(3, Inline(9), 0); // cp * 512
        break;
    default:
        c.VLshlrevB32(3, Inline(2), 0); // cp * 4, as the tracer hulls
        break;
    }
    const u32 count = kind == TracerHull::NoCover ? 5 : 6;
    for (u32 k = 0; k < count; ++k) {
        const u32 data = kind == TracerHull::MixedValues && k == 5 ? 5 : 4;
        c.BufferStoreDwordOffen(data, 3, k * 4, 8, 0);
    }
    c.SEndpgm();
    return c.words;
}

/// User data of a TracerHull: s[4:7] = a V# of the TessellationDataConstantBuffer (non-passthrough).
std::array<u32, 16> TracerHullUserData() {
    alignas(16) static Shader::TessellationDataConstantBuffer constants{};
    constants = {.ls_stride = 64, .hs_cp_stride = 16, .num_patches = 1, .hs_output_base = 128,
                 .patch_const_size = 96, .patch_const_base = 160, .patch_output_size = 32,
                 .off_chip_tessellation_factor_threshold = 1.0f,
                 .first_edge_tess_factor_index = 0};
    AmdGpu::Buffer sharp{};
    sharp.base_address = reinterpret_cast<u64>(&constants);
    sharp.num_records = sizeof(constants);
    sharp.data_format = u32(AmdGpu::DataFormat::Format32);
    sharp.num_format = u32(AmdGpu::NumberFormat::Uint);
    std::array<u32, 16> ud{};
    std::memcpy(&ud[4], &sharp, sizeof(sharp));
    return ud;
}

Shader::RuntimeInfo TracerHullRuntime() {
    Shader::RuntimeInfo rt{};
    rt.Initialize(Shader::HwStage::Hull, Shader::SwStage::TessellationControl);
    rt.props.num_user_data = 8;
    rt.props.num_allocated_vgprs = 64;
    rt.sw.tcs.num_threads = 2;
    rt.sw.tcs.num_input_control_points = 2;
    rt.sw.tcs.tess_type = AmdGpu::TessellationType::Quad;
    return rt;
}

/// The tracer shape compiles: tess factor stores at offsets that depend on the control point.
int CheckTracerHull(const std::filesystem::path& out);

/// GsEmulationUnsupported's stream-out ABI check: AmdGpu::InputUsageSlots on DeepSnow's copy
/// shader layout (slots 0x1a, 0x00020018, one padding dword, OrbShdr info with
/// chunk_usage_base_offset_in_dw 1), and SoTableAtS2 on that and on tables it must refuse.
int CheckSoTableSlots() {
    std::vector<u32> blob = {0xBF810000u /* s_endpgm */, 0x1a, 0x00020018, 0};
    AmdGpu::BinaryInfo info{};
    info.signature = AmdGpu::BinaryInfo::signature_ref;
    info.chunk_usage_base_offset_in_dw = 1;
    info.num_input_usage_slots = 2;
    const size_t at = blob.size();
    blob.resize(at + (sizeof(info) + 3) / 4 + 4);
    std::memcpy(&blob[at], &info, sizeof(info));
    const auto slots = AmdGpu::InputUsageSlots(blob.data());
    int failures = 0;
    const auto check = [&](const char* name, bool got, bool want) {
        std::printf("%-44s %s\n", name, got == want ? "PASS" : "FAIL");
        failures += got == want ? 0 : 1;
    };
    check("usage slots read before the binary info",
          slots.size() == 2 && slots[0] == 0x1a && slots[1] == 0x00020018, true);
    namespace GsEmu = Shader::GsEmulation;
    check("SO table at s2 (DeepSnow)", GsEmu::SoTableAtS2(slots), true);
    check("no SO table", GsEmu::SoTableAtS2(std::vector<u32>{0x1a}), false);
    check("SO table at s4", GsEmu::SoTableAtS2(std::vector<u32>{0x1a, 0x00040018}), false);
    check("other input at s2", GsEmu::SoTableAtS2(std::vector<u32>{0x0002001a}), false);
    check("SO table at s2, other input at s3",
          GsEmu::SoTableAtS2(std::vector<u32>{0x00020018, 0x00030016}), false);
    // Record-time binding check: V# stride must be the VGT stride, range >= VGT size.
    AmdGpu::Buffer v{};
    v.stride = 16;
    v.num_records = 4;
    check("V# 16 B x 4 holds VGT 16 dw / 4 dw", GsEmu::StreamOutBindingGap(v, 16, 4) == nullptr,
          true);
    check("V# 16 B x 4 one dword short of VGT 17 dw", GsEmu::StreamOutBindingGap(v, 17, 4) == nullptr,
          false);
    AmdGpu::Buffer wide = v;
    wide.stride = 32;
    wide.num_records = 2;
    check("V# stride 32 vs VGT stride 4 dw (same range)",
          GsEmu::StreamOutBindingGap(wide, 16, 4) == nullptr, false);
    AmdGpu::Buffer swizzled = v;
    swizzled.swizzle_enable = 1;
    check("swizzled V#", GsEmu::StreamOutBindingGap(swizzled, 16, 4) == nullptr, false);
    AmdGpu::Buffer huge = v;
    huge.num_records = 0x10000000;
    check("VGT size past 4 GiB (u64, no wrap)",
          GsEmu::StreamOutBindingGap(huge, 0x40000001u, 4) == nullptr, false);
    return failures;
}

/// The draw of a stream-out GS (CopyVertex, SO enabled) with Gnm's stream-out tail plus a ballot of
/// exec and a v_mbcnt rank: its stream-out stores go, and with them every wave operation, so the
/// vertex function declares no subgroup capability (Metal has none there). Compiled in-process,
/// the SPIR-V capabilities scanned.
int CheckCopyDrawWithoutSubgroups(const std::filesystem::path& out) {
    using namespace GsAsm;
    Code c;
    c.SMovLiteral(VCC_HI, 0);
    c.SLoadDwordx4(16, 2, 0);
    c.SLshlB32(6, 6, Inline(2));
    c.SBfeU32(11, 4, 16, 7);
    for (u32 v = 1; v <= 4; ++v) {
        c.VMov(v, Inline(0));
    }
    c.Exp(12, true, 1);
    c.LaneId(40);
    c.VMov(41, 11);
    c.VCmpLtU32(Vgpr(40), 41);
    c.SAndSaveexecB64(20, VCC_LO);
    c.VMov(42, 5);
    c.SMovB64(22, EXEC_LO);
    c.MaskRank(43, 22);
    c.VCmpEqU32(Vgpr(43), 40);
    c.SAndSaveexecB64(24, VCC_LO);
    c.BufferStoreDword(1, 42, true, 0, 6, 16);
    c.SOrSaveexecB64(20, 20);
    c.SEndpgm();
    alignas(16) static AmdGpu::Buffer table[1]{};
    table[0].base_address = 0x10000000;
    table[0].stride = 16;
    table[0].num_records = 64;
    table[0].add_tid_enable = 1;
    table[0].data_format = u32(AmdGpu::DataFormat::Format32_32_32_32);
    table[0].num_format = u32(AmdGpu::NumberFormat::Float);
    std::array<u32, 16> ud{};
    const u64 at = reinterpret_cast<u64>(&table[0]);
    ud[2] = u32(at);
    ud[3] = u32(at >> 32);
    Shader::RuntimeInfo rt{};
    rt.Initialize(Shader::HwStage::Vertex, Shader::SwStage::Vertex);
    rt.gs_role = Shader::GsEmulationRole::CopyVertex;
    rt.gs_max_vert_out = 1;
    rt.gs_vertex_dwords = 4;
    rt.gs_so_buffers = 1;
    rt.props.num_user_data = 4;
    rt.props.num_allocated_vgprs = 64;
    static Shader::Pools pools;
    const auto profile = AppleProfile();
    const Shader::ShaderParams params{.user_data = std::span<const u32, 16>(ud),
                                      .code = std::span<const u32>(c.words),
                                      .hash = 0x5d5d};
    Shader::Info info(rt.hw_stage, rt.sw_stage, params);
    Shader::Backend::Bindings binding{};
    const auto program = Shader::TranslateProgram(c.words, pools, info, rt, profile);
    const auto spv = Shader::Backend::SPIRV::EmitSPIRV(profile, rt, program, binding);
    std::ofstream(out / "copy-draw-so.spv", std::ios::binary)
        .write(reinterpret_cast<const char*>(spv.data()), std::streamsize(spv.size() * 4));
    std::vector<u32> subgroup_caps;
    for (size_t i = 5; i < spv.size();) {
        const u32 count = spv[i] >> 16, op = spv[i] & 0xFFFF;
        if (count == 0) {
            break;
        }
        // OpCapability: GroupNonUniform* (61-68), SubgroupBallotKHR, SubgroupVoteKHR.
        const u32 cap = op == 17 && count == 2 ? spv[i + 1] : 0;
        if ((cap >= 61 && cap <= 68) || cap == 4423 || cap == 4431) {
            subgroup_caps.push_back(cap);
        }
        i += count;
    }
    const bool pass = subgroup_caps.empty();
    std::printf("%-44s %s\n", "SO copy draw: no subgroup capability", pass ? "PASS" : "FAIL");
    for (const u32 cap : subgroup_caps) {
        std::printf("    OpCapability %u\n", cap);
    }
    return pass ? 0 : 1;
}

int CheckTracerHull(const std::filesystem::path& out) {
    // Compiled in this process: an assertion stops the controls (and run.sh) loudly.
    Compile("tracer-hull", TracerHullProgram(TracerHull::Tracer), TracerHullUserData(), 0x7ace,
            TracerHullRuntime(), out);
    std::printf("%-44s %s\n", "hull: tracer-shape tess factor stores", "PASS");
    return 0;
}

int RunNegatives(const std::string& self, const std::filesystem::path& dir,
                 const std::string& gs_hash, const std::filesystem::path& out, int only) {
    // DIR "-": a minimal synthetic copy shader (the GS role only parses it).
    std::vector<u32> copy_code;
    std::array<u32, 16> copy_ud{};
    if (dir == "-") {
        GsAsm::Code c;
        c.SMovLiteral(GsAsm::VCC_HI, 0);
        c.words.push_back(0xB0000000u | (4u << 16)); // s_movk_i32 s4, 0
        c.RingDword(false, 1, 0, true, 0, 4);
        c.SWaitcnt();
        c.Exp(12, true, 1);
        c.SEndpgm();
        copy_code = c.words;
    } else {
        copy_code = ReadWords(dir / ("copy_" + gs_hash + ".bin"));
    }
    const auto store = RingStore(0);
    const auto bad_store = RingStore(28 * 8 * 4); // component 28 of a 28-dword vertex, N = 8
    // Copy shaders: a store through a V# held in user data (not the s[2:3] stream-out table),
    // and a stream-out store (V# loaded through s[2:3]) in a draw without stream-out.
    GsAsm::Code user_vsharp_store;
    user_vsharp_store.BufferStoreDword(1, 0, false, 0, GsAsm::Inline(0), 0);
    user_vsharp_store.SEndpgm();
    // A store through the V# at dword `offset` of the s[2:3] table.
    const auto table_store = [](u32 offset) {
        GsAsm::Code c;
        c.SLoadDwordx4(16, 2, offset);
        c.SWaitcnt();
        c.BufferStoreDword(1, 0, true, 0, GsAsm::Inline(0), 16);
        c.SEndpgm();
        return c.words;
    };
    const auto so_store = table_store(0);
    std::vector<u32> lds_kernel = {DsReadB32[0], DsReadB32[1]}, barrier_kernel = {SBarrier};
    lds_kernel.insert(lds_kernel.end(), so_store.begin(), so_store.end());
    barrier_kernel.insert(barrier_kernel.end(), so_store.begin(), so_store.end());
    // Copy shaders whose GSVS load offset SGPR comes from the stream-out offset shift
    // (s_lshl_b32 s6, s6, 2: unknown to the copy-shader parse), through s_addk or s_mov.
    const auto unknown_offset_copy = [](bool through_mov) {
        GsAsm::Code c;
        c.SMovLiteral(GsAsm::VCC_HI, 0);
        c.SLshlB32(6, 6, GsAsm::Inline(2));
        u32 soffset = 6;
        if (through_mov) {
            c.Sop1(3, 7, 6); // s_mov_b32 s7, s6
            soffset = 7;
        } else {
            c.words.push_back(0xB0000000u | (15u << 23) | (6u << 16) | 4u); // s_addk_i32 s6, 4
        }
        c.RingDword(false, 1, 0, true, 0, soffset);
        c.SWaitcnt();
        c.Exp(12, true, 1);
        c.SEndpgm();
        return c.words;
    };
    // Stream-out kernel stores through table entry 0 at modified addresses.
    enum class SoAddress { AddkOffset, IndexFromConfig, UnshiftedOffset, PastVertex };
    const auto so_address_store = [](SoAddress how) {
        GsAsm::Code c;
        c.SLoadDwordx4(16, 2, 0);
        if (how != SoAddress::UnshiftedOffset) {
            c.SLshlB32(6, 6, GsAsm::Inline(2));
        }
        if (how == SoAddress::AddkOffset) {
            c.words.push_back(0xB0000000u | (15u << 23) | (6u << 16) | 4u); // s_addk_i32 s6, 4
        }
        c.VMov(42, how == SoAddress::IndexFromConfig ? 4 : 5);
        c.SWaitcnt();
        c.BufferStoreDword(1, 42, true, how == SoAddress::PastVertex ? 16 : 0, 6, 16);
        c.SEndpgm();
        return c.words;
    };
    // A copy shader exporting its lane id: the draw (a vertex function) has no lane id on Metal.
    GsAsm::Code lane_id;
    lane_id.LaneId(1);
    lane_id.VCvtF32U32(1, 1);
    lane_id.VMov(2, GsAsm::Inline(0));
    lane_id.VMov(3, GsAsm::Inline(0));
    lane_id.VMov(4, GsAsm::Inline(0));
    lane_id.Exp(12, true, 1);
    lane_id.SEndpgm();
    const auto lane_id_export = lane_id.words;
    const std::vector<Negative> cases = {
        {"ES s_barrier", Role::Es, {SBarrier, store[0], store[1], SEndpgm}, "uses s_barrier"},
        {"GS s_barrier", Role::Gs, {SBarrier, store[0], store[1], SEndpgm}, "uses s_barrier"},
        {"ES LDS read", Role::Es, {DsReadB32[0], DsReadB32[1], store[0], store[1], SEndpgm},
         "uses LDS"},
        {"GS LDS read", Role::Gs, {DsReadB32[0], DsReadB32[1], store[0], store[1], SEndpgm},
         "uses LDS"},
        {"ES v_writelane", Role::Es, {VWritelane, store[0], store[1], SEndpgm}, "v_writelane"},
        {"GS store past the vertex", Role::Gs, {bad_store[0], bad_store[1], SEndpgm},
         "another stream"},
        {"copy store, user-data V#", Role::Copy, user_vsharp_store.words,
         "not a stream-out buffer", 1},
        {"SO kernel store, user V#", Role::CopyStreamOut, user_vsharp_store.words,
         "not a stream-out buffer", 1},
        {"copy SO store, no SO", Role::Copy, so_store, "which the draw does not enable"},
        {"copy store, mixed V# dwords", Role::Copy, table_store(1), "not a stream-out buffer", 1},
        {"SO kernel, mixed V# dwords", Role::CopyStreamOut, table_store(2),
         "not a stream-out buffer", 1},
        {"copy store, disabled buffer", Role::Copy, table_store(4), "does not enable", 1},
        {"SO kernel, disabled buffer", Role::CopyStreamOut, table_store(8), "does not enable",
         0b1011},
        {"SO kernel, no SO buffers", Role::CopyStreamOut, so_store, "without stream-out buffers"},
        {"SO kernel LDS read", Role::CopyStreamOut, lds_kernel,
         "copy shader 0x1234 uses LoadSharedU32", 1},
        {"SO kernel s_barrier", Role::CopyStreamOut, barrier_kernel,
         "copy shader 0x1234 uses Barrier", 1},
        {"SO kernel: offset + s_addk", Role::CopyStreamOut, so_address_store(SoAddress::AddkOffset),
         "at an address other than", 1},
        {"SO kernel: index not s5", Role::CopyStreamOut,
         so_address_store(SoAddress::IndexFromConfig), "at an address other than", 1},
        {"SO kernel: offset not << 2", Role::CopyStreamOut,
         so_address_store(SoAddress::UnshiftedOffset), "at an address other than", 1},
        {"SO kernel: store past the vertex", Role::CopyStreamOut,
         so_address_store(SoAddress::PastVertex), "outside one vertex of its V#", 1, {}, true},
        {"copy draw: live lane id", Role::Copy, lane_id_export, "needs subgroup operations"},
        {"hull: upper cp unaligned factor", Role::Hull, TracerHullProgram(TracerHull::UpperUnaligned),
         "control point 1 writes byte 1"},
        {"hull: upper cp far factor", Role::Hull, TracerHullProgram(TracerHull::UpperFar),
         "control point 1 writes byte 512"},
        {"hull: factor values differ", Role::Hull, TracerHullProgram(TracerHull::MixedValues),
         "not one immediate"},
        {"hull: cp 0 misses a factor", Role::Hull, TracerHullProgram(TracerHull::NoCover),
         "does not write every factor"},
        {"GS copy: unknown SGPR + s_addk", Role::Gs, {store[0], store[1], SEndpgm},
         "GSVS load through SGPR 6 of unknown value", 0, unknown_offset_copy(false)},
        {"GS copy: unknown SGPR via s_mov", Role::Gs, {store[0], store[1], SEndpgm},
         "GSVS load through SGPR 7 of unknown value", 0, unknown_offset_copy(true)},
    };
    if (only >= 0) {
        // One case, in this process (the GPU log is written by a thread fork() would drop).
        const auto& c = cases.at(only);
        std::array<u32, 16> ud{};
        alignas(16) static AmdGpu::Buffer table[1]{};
        if (c.so_table) {
            table[0].base_address = 0x10000000;
            table[0].stride = 16;
            table[0].num_records = 64;
            table[0].add_tid_enable = 1;
            table[0].data_format = u32(AmdGpu::DataFormat::Format32_32_32_32);
            table[0].num_format = u32(AmdGpu::NumberFormat::Float);
            const u64 at = reinterpret_cast<u64>(&table[0]);
            ud[2] = u32(at);
            ud[3] = u32(at >> 32);
        }
        Shader::RuntimeInfo rt{};
        if (c.role == Role::Hull) {
            Compile("neg-hull", c.code, TracerHullUserData(), 0x1234, TracerHullRuntime(), out);
            return 0;
        }
        if (c.role == Role::Copy || c.role == Role::CopyStreamOut) {
            rt.Initialize(Shader::HwStage::Vertex, Shader::SwStage::Vertex);
            rt.gs_role = c.role == Role::Copy ? Shader::GsEmulationRole::CopyVertex
                                              : Shader::GsEmulationRole::CopyStreamOut;
            rt.gs_max_vert_out = 8;
            rt.gs_vertex_dwords = 28;
            rt.gs_so_buffers = c.so_buffers;
        } else if (c.role == Role::Gs) {
            rt.Initialize(Shader::HwStage::Geometry, Shader::SwStage::Geometry);
            rt.gs_role = Shader::GsEmulationRole::GsCompute;
            rt.gs_max_vert_out = 8;
            rt.gs_vertex_dwords = 28;
            rt.hw.gs.output_vertices = 8;
            rt.hw.gs.num_invocations = 1;
            rt.hw.gs.in_vertex_data_size = 28;
            rt.hw.gs.out_vertex_data_size = 28;
            rt.hw.gs.in_primitive = AmdGpu::PrimitiveType::PointList;
            rt.hw.gs.out_primitive.fill(AmdGpu::GsOutputPrimitiveType::TriangleStrip);
            rt.hw.gs.vs_copy = c.copy.empty() ? std::span<const u32>(copy_code)
                                              : std::span<const u32>(c.copy);
        } else {
            rt.Initialize(Shader::HwStage::Export, Shader::SwStage::Vertex);
            rt.gs_role = Shader::GsEmulationRole::EsCompute;
            rt.hw.es.vertex_data_size = 28;
        }
        // Copy shaders keep s4.. for their stream-out inputs (Gnm: 4 user SGPRs).
        const bool copy = c.role == Role::Copy || c.role == Role::CopyStreamOut;
        rt.props.num_user_data = copy ? 4 : 16;
        rt.props.num_allocated_vgprs = 256;
        Compile(c.role == Role::Gs ? "neg-gs" : c.role == Role::Es ? "neg-es" : "neg-copy", c.code,
                ud, 0x1234, rt, out);
        return 0;
    }
    int failures = CheckSoTableSlots() + CheckCopyDrawWithoutSubgroups(out) + CheckTracerHull(out);
    for (size_t i = 0; i < cases.size(); ++i) {
        const auto& c = cases[i];
        const auto log = out / ("negative-" + std::to_string(i) + ".log");
        const std::string command = "'" + self + "' --negative-case " + std::to_string(i) + " '" +
                                    dir.string() + "' " + gs_hash + " '" + out.string() + "' > '" +
                                    log.string() + "' 2>&1";
        const int status = std::system(command.c_str());
        std::ifstream f(log);
        const std::string text((std::istreambuf_iterator<char>(f)), {});
        const bool loud = !(WIFEXITED(status) && WEXITSTATUS(status) == 0);
        const bool named = text.find(c.expect) != std::string::npos;
        std::printf("%-28s %s\n", c.name,
                    loud && named ? "LOUD (expected)" : loud ? "LOUD, wrong message" : "COMPILED (FAIL)");
        if (!(loud && named)) {
            ++failures;
            std::printf("%s\n", text.c_str());
        }
    }
    (void)copy_ud;
    return failures ? 1 : 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 5 && std::string(argv[1]) == "--negative") {
        std::filesystem::create_directories(argv[4]);
        return RunNegatives(argv[0], argv[2], argv[3], argv[4], -1);
    }
    if (argc == 6 && std::string(argv[1]) == "--negative-case") {
        return RunNegatives(argv[0], argv[3], argv[4], argv[5], std::atoi(argv[2]));
    }
    if (argc < 6) {
        std::fprintf(stderr, "usage: compile_roles DIR ES_HASH GS_HASH OUT_DIR key=value...\n");
        return 2;
    }
    const std::filesystem::path dir = argv[1];
    const std::string es_hash = argv[2], gs_hash = argv[3];
    const std::filesystem::path out = argv[4];
    std::filesystem::create_directories(out);
    Config cfg;
    for (int i = 5; i < argc; ++i) {
        const std::string a = argv[i];
        const auto eq = a.find('=');
        cfg.kv[a.substr(0, eq)] = a.substr(eq + 1);
    }

    const auto load = [&](const char* stage, const std::string& hash, std::vector<u32>& code,
                          std::array<u32, 16>& ud) {
        const auto base = dir / (std::string(stage) + "_" + hash);
        code = ReadWords(base.string() + ".bin");
        const auto words = ReadWords(base.string() + ".ud");
        std::memcpy(ud.data(), words.data(), std::min<size_t>(words.size(), 16) * 4);
        MapUserDataPointers(ud);
    };
    std::vector<u32> es_code, gs_code, copy_code;
    std::array<u32, 16> es_ud{}, gs_ud{}, copy_ud{};
    load("es", es_hash, es_code, es_ud);
    load("gs", gs_hash, gs_code, gs_ud);
    load("copy", gs_hash, copy_code, copy_ud);

    const u32 max_vert_out = cfg.Get("max_vert_out");
    const u32 vert_dwords = cfg.Get("vert");

    Shader::RuntimeInfo es{};
    es.Initialize(Shader::HwStage::Export, Shader::SwStage::Vertex);
    es.gs_role = Shader::GsEmulationRole::EsCompute;
    es.props.num_user_data = 16;
    es.props.num_input_vgprs = cfg.Get("es_vgprs", 0);
    es.props.num_allocated_vgprs = 256;
    es.hw.es.vertex_data_size = cfg.Get("esgs");

    Shader::RuntimeInfo gs{};
    gs.Initialize(Shader::HwStage::Geometry, Shader::SwStage::Geometry);
    gs.gs_role = Shader::GsEmulationRole::GsCompute;
    gs.gs_max_vert_out = max_vert_out;
    gs.gs_vertex_dwords = vert_dwords;
    gs.props.num_user_data = 16;
    gs.props.num_allocated_vgprs = 256;
    gs.hw.gs.output_vertices = max_vert_out;
    gs.hw.gs.num_invocations = cfg.Get("instances", 1);
    gs.hw.gs.in_vertex_data_size = cfg.Get("esgs");
    gs.hw.gs.out_vertex_data_size = vert_dwords;
    gs.hw.gs.in_primitive = AmdGpu::PrimitiveType(cfg.Get("prim"));
    gs.hw.gs.out_primitive.fill(AmdGpu::GsOutputPrimitiveType(cfg.Get("out_prim")));
    gs.hw.gs.mode = AmdGpu::GsScenario::ScenarioG;
    gs.hw.gs.vs_copy = copy_code;
    gs.hw.gs.vs_copy_hash = std::strtoull(gs_hash.c_str(), nullptr, 16);

    Shader::RuntimeInfo copy{};
    copy.Initialize(Shader::HwStage::Vertex, Shader::SwStage::Vertex);
    copy.gs_role = Shader::GsEmulationRole::CopyVertex;
    copy.gs_max_vert_out = max_vert_out;
    copy.gs_vertex_dwords = vert_dwords;
    copy.props.num_user_data = 16;
    copy.props.num_allocated_vgprs = 256;
    copy.hw.vs.num_outputs = 0;

    Compile("es", es_code, es_ud, std::strtoull(es_hash.c_str(), nullptr, 16), es, out);
    Compile("gs", gs_code, gs_ud, std::strtoull(gs_hash.c_str(), nullptr, 16), gs, out);
    Compile("copy", copy_code, copy_ud, std::strtoull(gs_hash.c_str(), nullptr, 16), copy, out);
    return 0;
}
