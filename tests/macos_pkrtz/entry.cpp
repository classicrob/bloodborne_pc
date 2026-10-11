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
    assert(argc == 3);
    const u32 mode = u32(std::atoi(argv[2]));

    Info info{};
    info.hw_stage = HwStage::Compute;
    info.sw_stage = SwStage::Compute;
    info.pgm_hash = 0x2f16;

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
    const IR::U32 other{ir.LoadBufferU32(1,ir.Imm32(0),ir.Imm32(4),flags)};
    const auto pair=ir.CompositeConstruct(ir.BitCast<IR::F32>(input),ir.BitCast<IR::F32>(other));
    const auto output=ir.PackHalf2x16Rtz(pair);
    ir.StoreBufferU32(1, ir.Imm32(0), ir.Imm32(8), output, flags);
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
    runtime.props.fp_round_mode16_64 = static_cast<AmdGpu::FpRoundMode>(3);
    Profile profile{};
    profile.supported_spirv = 0x00010500;

    profile.support_fp32_signed_zero_inf_nan_preserve = true;
    profile.support_fp32_denorm_flush = true;
    Backend::Bindings bindings{};
    const auto words = Backend::SPIRV::EmitSPIRV(profile, runtime, program, bindings);
    unsigned integer_shifts=0;
    for(size_t i=5;i<words.size();i+=words[i]>>16) {
        const auto op=static_cast<spv::Op>(words[i]&0xffff);
        assert(op!=spv::Op::OpFConvert);
        if(op==spv::Op::OpExtInst) assert(words[i+4]!=58u); // GLSL PackHalf2x16
        integer_shifts+=op==spv::Op::OpShiftRightLogical;
    }
    assert(integer_shifts>=8);
    std::ofstream file(argv[1], std::ios::binary);
    file.write(reinterpret_cast<const char*>(words.data()), words.size() * sizeof(u32));
    assert(file.good());
}
