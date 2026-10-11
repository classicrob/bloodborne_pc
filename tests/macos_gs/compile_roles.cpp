// SPDX-License-Identifier: GPL-2.0-or-later
// Geometry emulation (Apple): compiles a dumped ES/GS/copy-shader triplet (the gs_report files the
// GS abort writes) through the production recompiler in the three emulation roles (EsCompute,
// GsCompute, CopyVertex), writes the SPIR-V, and lists each role's buffers. spirv-val runs from
// build.sh. The configuration comes from the "Geometry configuration:" log line.
//
//   compile_roles DIR ES_HASH GS_HASH OUT_DIR prim=N out_prim=N max_vert_out=N esgs=N vert=N
//                 [instances=N]
//   compile_roles --negative DIR|- GS_HASH OUT_DIR
//       compile-negative controls: hand-encoded ES/GS programs using what the compute roles cannot
//       run must stop loudly (each in a child process), naming the feature.
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
#include "shader_recompiler/info.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/runtime_info.h"
#include "tests/macos_gs/gs_asm.h"

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

struct Negative {
    const char* name;
    bool gs;
    std::vector<u32> code;
    const char* expect;
};

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
    const std::vector<Negative> cases = {
        {"ES s_barrier", false, {SBarrier, store[0], store[1], SEndpgm}, "uses s_barrier"},
        {"GS s_barrier", true, {SBarrier, store[0], store[1], SEndpgm}, "uses s_barrier"},
        {"ES LDS read", false, {DsReadB32[0], DsReadB32[1], store[0], store[1], SEndpgm},
         "uses LDS"},
        {"GS LDS read", true, {DsReadB32[0], DsReadB32[1], store[0], store[1], SEndpgm},
         "uses LDS"},
        {"ES v_writelane", false, {VWritelane, store[0], store[1], SEndpgm}, "v_writelane"},
        {"GS store past the vertex", true, {bad_store[0], bad_store[1], SEndpgm},
         "another stream"},
    };
    if (only >= 0) {
        // One case, in this process (the GPU log is written by a thread fork() would drop).
        const auto& c = cases.at(only);
        std::array<u32, 16> ud{};
        Shader::RuntimeInfo rt{};
        if (c.gs) {
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
            rt.hw.gs.vs_copy = copy_code;
        } else {
            rt.Initialize(Shader::HwStage::Export, Shader::SwStage::Vertex);
            rt.gs_role = Shader::GsEmulationRole::EsCompute;
            rt.hw.es.vertex_data_size = 28;
        }
        rt.props.num_user_data = 16;
        rt.props.num_allocated_vgprs = 256;
        Compile(c.gs ? "neg-gs" : "neg-es", c.code, ud, 0x1234, rt, out);
        return 0;
    }
    int failures = 0;
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
