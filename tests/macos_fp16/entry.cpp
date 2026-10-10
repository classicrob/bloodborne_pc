// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/program.h"
#include <spirv/unified1/spirv.hpp11>

using namespace Shader;

int main(int argc, char** argv) {
    assert(argc == 5);
    const u32 mode = u32(std::atoi(argv[2]));
    const u32 rounding = u32(std::atoi(argv[3]));
    const u32 operation = u32(std::atoi(argv[4]));
    Info info{};
    info.hw_stage = HwStage::Compute;
    info.sw_stage = SwStage::Compute;
    info.pgm_hash = 0xf160;
    info.uses_fp16 = true;
    auto& resource = info.buffers.emplace_back();
    resource.used_types = IR::Type::U32;
    resource.is_written = true;
    const auto sharp = AmdGpu::Buffer::Placeholder(64);
    std::memcpy(resource.sharp_fetch.immediates.data(), &sharp, sizeof(sharp));
    Common::ObjectPool<IR::Inst> pool;
    IR::Block block(pool);
    IR::IREmitter ir(block);
    ir.Prologue();
    IR::BufferInstInfo flags{};
    const IR::U32 input{ir.LoadBufferU32(1, ir.Imm32(0), ir.Imm32(0), flags)};
    IR::U32 output;
    if (operation == 0) {
        const IR::F16 half = ir.FPConvert(16, ir.BitCast<IR::F32>(input));
        output = ir.UConvert(32, ir.BitCast<IR::U16>(half));
    } else {
        const IR::U16 bits = ir.UConvert(16, input);
        const auto half = ir.BitCast<IR::F16>(bits);
        output = operation == 1 ? ir.BitCast<IR::U32>(IR::F32{ir.FPConvert(32, half)})
                                : IR::U32{ir.UConvert(32, ir.BitCast<IR::U16>(half))};
    }
    ir.StoreBufferU32(1, ir.Imm32(0), ir.Imm32(4), output, flags);
    ir.Epilogue();
    IR::Program program(info);
    program.blocks.push_back(&block);
    program.syntax_list.push_back({.data = {.block = &block},
                                   .type = IR::AbstractSyntaxNode::Type::Block});
    program.syntax_list.push_back({.type = IR::AbstractSyntaxNode::Type::Return});
    RuntimeInfo runtime{};
    runtime.Initialize(HwStage::Compute, SwStage::Compute);
    runtime.hw.cs.workgroup_size = {1, 1, 1};
    runtime.props.fp_denorm_mode32 = AmdGpu::FpDenormMode::InOutFlush;
    runtime.props.fp_denorm_mode16_64 = static_cast<AmdGpu::FpDenormMode>(mode);
    runtime.props.fp_round_mode16_64 = static_cast<AmdGpu::FpRoundMode>(rounding);
    Profile profile{};
    profile.supported_spirv = 0x00010500;
    profile.support_float16 = true;
    profile.support_fp16_signed_zero_inf_nan_preserve = true;
    profile.support_fp32_signed_zero_inf_nan_preserve = true;
    profile.support_fp32_denorm_flush = true;
    Backend::Bindings bindings{};
    const auto words = Backend::SPIRV::EmitSPIRV(profile, runtime, program, bindings);
    // No width-wide half denorm/rounding mode may override the per-opcode implementation.
    for (size_t i = 5; i < words.size(); i += words[i] >> 16) {
        assert(words[i] >> 16);
        if ((words[i] & 0xffff) != u32(spv::Op::OpExecutionMode) || (words[i] >> 16) < 4)
            continue;
        if (words[i + 3] != 16) continue;
        assert(words[i + 2] != u32(spv::ExecutionMode::DenormPreserve));
        assert(words[i + 2] != u32(spv::ExecutionMode::DenormFlushToZero));
        assert(words[i + 2] != u32(spv::ExecutionMode::RoundingModeRTZ));
    }
    std::ofstream file(argv[1], std::ios::binary);
    file.write(reinterpret_cast<const char*>(words.data()), words.size() * sizeof(u32));
    assert(file.good());
}
