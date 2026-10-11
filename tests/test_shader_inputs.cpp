// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sys/wait.h>
#include <unistd.h>
#include "gcn_buffer_conformance/gcn_asm.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/recompiler.h"
#include <spirv/unified1/spirv.hpp11>

using namespace Shader;

static Profile TestProfile() {
    Profile profile{};
    profile.supported_spirv = 0x00010600;
    profile.subgroup_size = 32;
    profile.support_int64 = true;
    profile.support_int8 = true;
    profile.support_int16 = true;
    profile.support_fp32_denorm_flush = true;
    profile.support_fp32_signed_zero_inf_nan_preserve = true;
    return profile;
}
static void Save(const std::filesystem::path& path, const std::vector<u32>& words) {
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(words.data()), words.size() * sizeof(u32));
    assert(file.good());
}
static void SubgroupInputs(const std::filesystem::path& out, HwStage hw, SwStage sw) {
    Info info{};
    info.hw_stage = hw;
    info.sw_stage = sw;
    info.uses_lane_id = true;
    info.loads.Set(IR::Attribute::SubgroupLtMask);
    RuntimeInfo runtime{};
    runtime.Initialize(hw, sw);
    if (hw == HwStage::Compute) runtime.hw.cs.workgroup_size = {16, 1, 1};
    Common::ObjectPool<IR::Inst> pool;
    IR::Block block(pool);
    IR::IREmitter ir(block);
    ir.Prologue();
    ir.Epilogue();
    IR::Program program(info);
    program.blocks.push_back(&block);
    program.syntax_list.push_back({.data = {.block = &block},
                                  .type = IR::AbstractSyntaxNode::Type::Block});
    program.syntax_list.push_back({.type = IR::AbstractSyntaxNode::Type::Return});
    Backend::Bindings binding{};
    const auto words = Backend::SPIRV::EmitSPIRV(TestProfile(), runtime, program, binding);
    std::set<u32> builtins, flat;
    for (size_t i = 5; i < words.size(); i += words[i] >> 16) {
        assert((words[i] >> 16) && i + (words[i] >> 16) <= words.size());
        if ((words[i] & 0xffff) != u32(spv::Op::OpDecorate)) continue;
        if (words[i + 2] == u32(spv::Decoration::BuiltIn) &&
            (words[i + 3] == u32(spv::BuiltIn::SubgroupLocalInvocationId) ||
             words[i + 3] == u32(spv::BuiltIn::SubgroupLtMask))) builtins.insert(words[i + 1]);
        if (words[i + 2] == u32(spv::Decoration::Flat)) flat.insert(words[i + 1]);
    }
    assert(builtins.size() == 2);
    for (const auto id : builtins) assert(flat.contains(id) == (hw == HwStage::Fragment));
    Save(out / ("subgroup-" + std::to_string(u32(hw)) + ".spv"), words);
}
static void QueryLod(const std::filesystem::path& out, bool null_image) {
    GcnAsm::Code code;
    code.VMovLiteral(0, 0x3f000000);
    code.VMovLiteral(1, 0x3f000000);
    // image_get_lod v[4:5], v[0:1], s[0:7], s[8:11] dmask:3
    code.words.push_back(0xf0000000u | (96u << 18) | (3u << 8));
    code.words.push_back((4u << 8) | (2u << 21));
    code.SWaitcntAll();
    code.Mubuf(GcnAsm::BUFFER_STORE_DWORDX2, {.vdata = 4, .srsrc = 12});
    code.SEndpgm();
    auto image = AmdGpu::Image::Null(false);
    image.base_address = null_image ? 0 : 0x2000000000ull >> 8;
    image.width = image.height = 31;
    AmdGpu::Sampler sampler{};
    sampler.max_lod.Assign(0x345);
    auto output = AmdGpu::Buffer::Placeholder(64);
    output.base_address = 0x3000000000ull;
    std::array<u32, 16> ud{};
    std::memcpy(ud.data(), &image, sizeof(image));
    std::memcpy(ud.data() + 8, &sampler, sizeof(sampler));
    std::memcpy(ud.data() + 12, &output, sizeof(output));
    ShaderParams params{.user_data = ud, .code = code.words, .hash = 0xf9c8eb4a};
    Info info(HwStage::Fragment, SwStage::Fragment, params);
    RuntimeInfo runtime{};
    runtime.Initialize(HwStage::Fragment, SwStage::Fragment);
    runtime.props.num_user_data = 16;
    runtime.props.num_allocated_vgprs = 16;
    auto profile = TestProfile();
    profile.emulate_null_descriptors = true;
    Pools pools;
    const auto program = TranslateProgram(code.words, pools, info, runtime, profile);
    assert(info.images.size() == 1 && info.samplers.size() == 1);
    assert(info.samplers[0].GetSharp(info).max_lod.Value() == 0x345);
    Backend::Bindings binding{};
    const auto words = Backend::SPIRV::EmitSPIRV(profile, runtime, program, binding);
    unsigned queries = 0;
    for (size_t i = 5; i < words.size(); i += words[i] >> 16) {
        assert((words[i] >> 16) && i + (words[i] >> 16) <= words.size());
        const auto opcode = spv::Op(words[i] & 0xffff);
        if (opcode == spv::Op::OpLoad) assert((words[i] >> 16) >= 4);
        queries += opcode == spv::Op::OpImageQueryLod;
    }
    assert(queries == unsigned(!null_image));
    Save(out / ("query-lod-" + std::to_string(null_image) + ".spv"), words);
}
struct ParameterCase {
    const char* name;
    u32 selector = 2;
    bool flat = true;
    bool is_default = false;
    bool dead = false;
    bool dead_p10 = false;
    bool mixed_p10 = false;
    bool native = false;
};
static void Parameter(const std::filesystem::path& out, const ParameterCase& c) {
    GcnAsm::Code code;
    const auto mov = [&](u32 selector, u32 dst) {
        code.words.push_back(0xc8000000u | (dst << 18) | (2u << 16) | (2u << 10) | selector);
    };
    if (c.dead_p10 || c.mixed_p10) mov(0, 9);
    mov(c.selector, 8);
    if (c.dead) code.VMovLiteral(8, 0x3f800000);
    code.Mubuf(GcnAsm::BUFFER_STORE_DWORD, {.vdata = 8, .srsrc = 0});
    if (c.mixed_p10) code.Mubuf(GcnAsm::BUFFER_STORE_DWORD, {.offset = 4, .vdata = 9, .srsrc = 0});
    code.SEndpgm();
    auto output = AmdGpu::Buffer::Placeholder(64);
    output.base_address = 0x3000000000ull;
    std::array<u32, 16> ud{};
    std::memcpy(ud.data(), &output, sizeof(output));
    ShaderParams params{.user_data = ud, .code = code.words, .hash = 0xb6e87124};
    Info info(HwStage::Fragment, SwStage::Fragment, params);
    RuntimeInfo runtime{};
    runtime.Initialize(HwStage::Fragment, SwStage::Fragment);
    runtime.props.num_user_data = 4;
    runtime.props.num_allocated_vgprs = 16;
    runtime.hw.fs.num_inputs = 3;
    runtime.hw.fs.inputs[0].is_default = true;
    runtime.hw.fs.inputs[1].is_default = true;
    runtime.hw.fs.inputs[2] = {.param_index = 5, .is_default = c.is_default,
                               .is_flat = c.flat, .default_value = 3};
    auto profile = TestProfile();
    profile.supports_fragment_shader_barycentric = true;
    profile.supports_fragment_per_vertex_inputs = c.native;
    Pools pools;
    const auto program = TranslateProgram(code.words, pools, info, runtime, profile);
    const bool live = !c.dead && !(c.is_default && !c.flat);
    assert(info.loads.GetAny(IR::Attribute::Param2) == live);
    if (live) assert(info.fs_interpolation[2].primary ==
                     (c.native ? Qualifier::PerVertex : Qualifier::Flat));
    Backend::Bindings binding{};
    const auto words = Backend::SPIRV::EmitSPIRV(profile, runtime, program, binding);
    u32 per_vertex = 0, flat = 0;
    for (size_t i = 5; i < words.size(); i += words[i] >> 16) {
        assert((words[i] >> 16) && i + (words[i] >> 16) <= words.size());
        if ((words[i] & 0xffff) != u32(spv::Op::OpDecorate)) continue;
        per_vertex += words[i + 2] == u32(spv::Decoration::PerVertexKHR);
        flat += words[i + 2] == u32(spv::Decoration::Flat);
    }
    assert(per_vertex == u32(c.native && live));
    assert(flat == u32(!c.native && live));
    Save(out / (std::string(c.name) + ".spv"), words);
}
static void RejectParameter(const std::filesystem::path& out, const ParameterCase& c) {
    int output[2]; assert(pipe(output) == 0);
    std::fflush(nullptr);
    const pid_t child = fork(); assert(child >= 0);
    if (!child) {
        close(output[0]);
        assert(dup2(output[1], STDERR_FILENO) >= 0);
        close(output[1]);
        Parameter(out, c);
        _Exit(0);
    }
    close(output[1]);
    std::string message;
    char bytes[1024];
    ssize_t count;
    while ((count = read(output[0], bytes, sizeof(bytes))) > 0) message.append(bytes, count);
    assert(count == 0); close(output[0]);
    int status; assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 23);
    assert(message.find("cannot be lowered to Flat") != std::string::npos);
    assert(message.find("0xb6e87124") != std::string::npos);
    std::printf("PASS parameter rejection %s\n", c.name);
}
int main(int argc, char** argv) {
    assert(argc == 2);
    const std::filesystem::path out = argv[1];
    std::filesystem::create_directories(out);
    SubgroupInputs(out, HwStage::Vertex, SwStage::Vertex);
    SubgroupInputs(out, HwStage::Fragment, SwStage::Fragment);
    SubgroupInputs(out, HwStage::Compute, SwStage::Compute);
    QueryLod(out, false);
    QueryLod(out, true);
    Parameter(out, {.name = "flat-p0"});
    Parameter(out, {.name = "default-p0", .flat = false, .is_default = true});
    Parameter(out, {.name = "dead-nonflat-p0", .flat = false, .dead = true});
    Parameter(out, {.name = "dead-passthrough-p0", .is_default = true, .dead = true});
    Parameter(out, {.name = "dead-p10-live-p0", .dead_p10 = true});
    Parameter(out, {.name = "native-p0", .flat = false, .native = true});
    Parameter(out, {.name = "native-p10", .selector = 0, .flat = false, .native = true});
    RejectParameter(out, {.name = "nonflat-p0", .flat = false});
    RejectParameter(out, {.name = "passthrough-p0", .is_default = true});
    RejectParameter(out, {.name = "p10", .selector = 0});
    RejectParameter(out, {.name = "p20", .selector = 1});
    RejectParameter(out, {.name = "p10-then-p0", .mixed_p10 = true});
    std::puts("PASS: stage-appropriate subgroup inputs and encoded IMAGE_GET_LOD sampler tracking");
}
