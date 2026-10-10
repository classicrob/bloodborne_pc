// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/specialization.h"
#include <spirv/unified1/spirv.hpp11>

using namespace Shader;

static void Compile(const std::string& output, bool enabled, u32 start, bool store, u8 samples) {
    Info info{};
    info.hw_stage = HwStage::Fragment;
    info.sw_stage = SwStage::Fragment;
    info.pgm_hash = 0xdb000 + start;
    if (store) {
        auto& resource = info.buffers.emplace_back();
        resource.used_types = IR::Type::U32;
        resource.is_written = true;
        const auto sharp = AmdGpu::Buffer::Placeholder(64);
        std::memcpy(resource.sharp_fetch.immediates.data(), &sharp, sizeof(sharp));
    }
    RuntimeInfo runtime{};
    runtime.Initialize(HwStage::Fragment, SwStage::Fragment);
    runtime.hw.fs.depth_bounds_test = enabled;
    runtime.hw.fs.num_samples = samples;
    Profile profile{};
    profile.supported_spirv = 0x00010500;
    profile.support_fp32_denorm_flush = true;
    profile.support_fp32_signed_zero_inf_nan_preserve = true;
    Backend::Bindings bindings{.unified = start};

    StageSpecialization first(info, runtime, profile, bindings);
    auto ordinary = runtime;
    ordinary.hw.fs.depth_bounds_test = !enabled;
    StageSpecialization changed(info, ordinary, profile, bindings);
    assert(first != changed);
    if (enabled) {
        StageSpecialization moved(info, runtime, profile, {.unified = start + 1});
        assert(first != moved);
    }

    Common::ObjectPool<IR::Inst> pool;
    IR::Block block(pool);
    IR::IREmitter ir(block);
    // Deliberately omit Prologue: the test must precede even the first guest instruction.
    if (store) {
        ir.StoreBufferU32(1, ir.Imm32(0), ir.Imm32(0), ir.Imm32(0x12345678), {});
    }
    ir.Epilogue();
    IR::Program program(info);
    program.blocks.push_back(&block);
    program.syntax_list.push_back({.data = {.block = &block},
                                   .type = IR::AbstractSyntaxNode::Type::Block});
    program.syntax_list.push_back({.type = IR::AbstractSyntaxNode::Type::Return});
    const auto words = Backend::SPIRV::EmitSPIRV(profile, runtime, program, bindings);
    assert(bindings.unified == start + u32(store));
    assert(bindings.buffer == u32(store));
    assert(info.depth_bounds_binding == (enabled ? 0u : std::numeric_limits<u32>::max()));
    auto cached_bindings = Backend::Bindings{.unified = start};
    info.AddBindings(cached_bindings);
    assert(cached_bindings == bindings);

    size_t fetch = 0, kill = 0, first_store = 0;
    u32 depth_variable = 0;
    bool minimum_offset = false, maximum_offset = false;
    for (size_t i = 5; i < words.size(); i += words[i] >> 16) {
        assert(words[i] >> 16);
        const auto op = static_cast<spv::Op>(words[i] & 0xffff);
        if (op == spv::Op::OpExecutionMode) {
            assert(words[i + 2] != u32(spv::ExecutionMode::EarlyFragmentTests));
        }
        if (op == spv::Op::OpDecorate && words[i + 2] == u32(spv::Decoration::DescriptorSet) &&
            words[i + 3] == 1) {
            assert(!depth_variable);
            depth_variable = words[i + 1];
        }
        if (op == spv::Op::OpMemberDecorate && words[i + 3] == u32(spv::Decoration::Offset)) {
            minimum_offset |= words[i + 2] == PushData::DepthBoundsMinIndex && words[i + 4] == 320;
            maximum_offset |= words[i + 2] == PushData::DepthBoundsMaxIndex && words[i + 4] == 324;
        }
        if (op == spv::Op::OpImageFetch) { assert(!fetch); fetch = i; }
        if (op == spv::Op::OpKill) { assert(!kill); kill = i; }
        if (op == spv::Op::OpStore && !first_store) first_store = i;
        assert(op != spv::Op::OpDemoteToHelperInvocationEXT);
    }
    assert(minimum_offset && maximum_offset);
    assert(bool(fetch) == enabled && bool(kill) == enabled && bool(depth_variable) == enabled);
    if (enabled) {
        bool binding_zero = false;
        for (size_t i = 5; i < words.size(); i += words[i] >> 16) {
            if ((words[i] & 0xffff) == u32(spv::Op::OpDecorate) && words[i + 1] == depth_variable &&
                words[i + 2] == u32(spv::Decoration::Binding)) {
                binding_zero = words[i + 3] == 0;
            }
        }
        assert(binding_zero && fetch < kill && (!first_store || kill < first_store));
    }
    const auto filename = output + "/depth-" + std::to_string(enabled) + "-" +
                          std::to_string(start) + "-" + std::to_string(store) + ".spv";
    std::ofstream file(filename, std::ios::binary);
    file.write(reinterpret_cast<const char*>(words.data()), words.size() * sizeof(u32));
    assert(file.good());
    std::printf("PASS depth shader enabled=%u start=%u side_effect=%u\n", enabled, start, store);
}

int main(int argc, char** argv) {
    assert(argc == 2 || argc == 3);
    if (argc == 3) {
        Compile(argv[1], true, 0, false, 4);
        return 2;
    }
    for (bool enabled : {false, true})
        for (u32 start : {0u, 7u})
            for (bool store : {false, true})
                Compile(argv[1], enabled, start, store, 1);
}
