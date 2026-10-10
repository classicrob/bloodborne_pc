// SPDX-License-Identifier: GPL-2.0-or-later
// Geometry emulation (Apple): compiles a dumped ES/GS/copy-shader triplet (the gs_report files the
// GS abort writes) through the production recompiler in the three emulation roles (EsCompute,
// GsCompute, CopyVertex), writes the SPIR-V, and lists each role's buffers. spirv-val runs from
// build.sh. The configuration comes from the "Geometry configuration:" log line.
//
//   compile_roles DIR ES_HASH GS_HASH OUT_DIR prim=N out_prim=N max_vert_out=N esgs=N vert=N
//                 [index=none|u16|u32] [instances=N]
#include <mach/mach.h>
#include <mach/mach_vm.h>
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

} // namespace

int main(int argc, char** argv) {
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
    const auto index = cfg.kv.count("index") ? cfg.kv["index"] : std::string("none");

    Shader::RuntimeInfo es{};
    es.Initialize(Shader::HwStage::Export, Shader::SwStage::Vertex);
    es.gs_role = Shader::GsEmulationRole::EsCompute;
    es.gs_index_type = index == "u16"   ? Shader::GsIndexType::U16
                       : index == "u32" ? Shader::GsIndexType::U32
                                        : Shader::GsIndexType::None;
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
