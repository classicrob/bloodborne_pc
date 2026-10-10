// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <cassert>
#include <cstdio>
#include <fstream>
#include <cstdlib>
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"
#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/ir/microinstruction.h"
using namespace Shader;
using namespace Shader::Backend::SPIRV;

static std::vector<u32> Emit(u32 mode, bool rtz) {
    Profile profile{};
    profile.supported_spirv = 0x00010500;

    Info info{};
    info.hw_stage = HwStage::Compute;
    info.sw_stage = SwStage::Compute;

    RuntimeInfo runtime{};
    runtime.props.fp_denorm_mode32 = AmdGpu::FpDenormMode::InOutFlush;
    runtime.props.fp_denorm_mode16_64 = static_cast<AmdGpu::FpDenormMode>(mode);

    Backend::Bindings bindings{};
    EmitContext ctx(profile, runtime, info, bindings);
    ctx.SetMemoryModel(spv::AddressingModel::PhysicalStorageBuffer64, spv::MemoryModel::GLSL450);
    ctx.AddCapability(spv::Capability::Int8);
    ctx.AddCapability(spv::Capability::Int16);
    ctx.AddCapability(spv::Capability::Int64);
    ctx.AddCapability(spv::Capability::PhysicalStorageBufferAddresses);
    const auto gid = ctx.AddGlobalVariable(ctx.TypePointer(spv::StorageClass::Input, ctx.U32[3]),
                                           spv::StorageClass::Input);
    ctx.Decorate(gid, spv::Decoration::BuiltIn, u32(spv::BuiltIn::GlobalInvocationId));
    const auto push_type = ctx.TypeStruct(ctx.U64);
    ctx.Decorate(push_type, spv::Decoration::Block);
    ctx.MemberDecorate(push_type, 0, spv::Decoration::Offset, 0u);
    const auto push = ctx.AddGlobalVariable(ctx.TypePointer(spv::StorageClass::PushConstant, push_type),
                                            spv::StorageClass::PushConstant);
    const auto push_ptr = ctx.TypePointer(spv::StorageClass::PushConstant, ctx.U64);
    const auto word_ptr = ctx.TypePointer(spv::StorageClass::PhysicalStorageBuffer, ctx.U32[1]);
    const auto function = ctx.OpFunction(ctx.void_id, spv::FunctionControlMask::MaskNone,
                                         ctx.TypeFunction(ctx.void_id));
    ctx.AddLabel();
    const auto index = ctx.OpCompositeExtract(ctx.U32[1], ctx.OpLoad(ctx.U32[3], gid), 0u);
    const auto base = ctx.OpLoad(ctx.U64, ctx.OpAccessChain(push_ptr, push, ctx.u32_zero_value));
    const auto address = ctx.OpIAdd(ctx.U64, base, ctx.OpUConvert(ctx.U64,
        ctx.OpIMul(ctx.U32[1], index, ctx.ConstU32(32u))));
    const auto pointer = [&](u32 word) {
        return ctx.OpConvertUToPtr(word_ptr, ctx.OpIAdd(ctx.U64, address,
            ctx.Constant(ctx.U64, u64(word * 4))));
    };
    const auto load = [&](u32 word) {
        return ctx.OpLoad(ctx.U32[1], pointer(word), spv::MemoryAccessMask::Aligned, 4u);
    };
    IR::Inst instruction(IR::Opcode::PackHalf2x16, rtz ? 1u : 0u);
    const auto pair=ctx.OpCompositeConstruct(ctx.F32[2],
        ctx.OpBitcast(ctx.F32[1],load(1)),ctx.OpBitcast(ctx.F32[1],load(2)));
    const auto output=EmitPackHalf2x16(ctx,&instruction,pair);
    ctx.OpStore(pointer(4),output,spv::MemoryAccessMask::Aligned,4u);
    ctx.OpReturn();
    ctx.OpFunctionEnd();
    const std::array interfaces{gid, push};
    ctx.AddEntryPoint(spv::ExecutionModel::GLCompute, function, "main", interfaces);
    ctx.AddExecutionMode(function, spv::ExecutionMode::LocalSize, 1u, 1u, 1u);
    return ctx.Assemble();
}

int main(int argc, char** argv) {
    assert(argc==2 || argc==3);
    const bool rtz=argc==2;
    for(u32 mode=0;mode<4;++mode) {
        const auto code=Emit(mode,rtz);
        std::ofstream file(std::string(argv[1])+"/mode-"+std::to_string(mode)+".spv",std::ios::binary);
        file.write(reinterpret_cast<const char*>(code.data()),code.size()*sizeof(u32));
        assert(file.good());
    }
    std::puts(rtz ? "Production integer RTZ pack shaders emitted" : "Native RTE pack negative controls emitted");
}
