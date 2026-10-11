// Exercise the production arithmetic emitters with runtime-fed buffer operands.
#include <array>
#include <cassert>
#include <cstdio>
#include <fstream>
#include <vector>
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"
#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
using namespace Shader;
using namespace Shader::Backend::SPIRV;

static std::vector<u32> Emit(bool repair, bool benchmark) {
    Profile profile{};
    profile.supported_spirv = 0x00010500;
    profile.repair_mul_signed_zero = repair;
    Info info{};
    info.hw_stage = HwStage::Compute;
    info.sw_stage = SwStage::Compute;
    RuntimeInfo runtime{};
    Backend::Bindings bindings{};
    EmitContext ctx(profile, runtime, info, bindings);
    ctx.SetMemoryModel(spv::AddressingModel::PhysicalStorageBuffer64, spv::MemoryModel::GLSL450);
    ctx.AddCapability(spv::Capability::Int8);
    ctx.AddCapability(spv::Capability::Int16);
    ctx.AddCapability(spv::Capability::Int64);
    ctx.AddCapability(spv::Capability::PhysicalStorageBufferAddresses);
    ctx.AddCapability(spv::Capability::SignedZeroInfNanPreserve);
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
        return ctx.OpConvertUToPtr(word_ptr, ctx.OpIAdd(ctx.U64, address, ctx.Constant(ctx.U64, u64(word * 4))));
    };
    const auto load = [&](u32 word) {
        return ctx.OpLoad(ctx.U32[1], pointer(word), spv::MemoryAccessMask::Aligned, 4u);
    };
    const auto a_bits = load(1), b_bits = load(2), c_bits = load(3);
    const auto a = ctx.OpBitcast(ctx.F32[1], a_bits);
    const auto b = ctx.OpBitcast(ctx.F32[1], b_bits);
    const auto c = ctx.OpBitcast(ctx.F32[1], c_bits);
    const auto store = [&](Id bits) {
        ctx.OpStore(pointer(4), bits, spv::MemoryAccessMask::Aligned, 4u);
    };
    const auto sign = [&] {
        return ctx.OpBitwiseAnd(ctx.U32[1], ctx.OpBitwiseXor(ctx.U32[1], a_bits, b_bits), ctx.ConstU32(0x80000000u));
    };
    if (benchmark) {
        auto product = a;
        for (u32 i = 0; i < 128; ++i) product = EmitFPMul32(ctx, nullptr, product, b);
        store(ctx.OpBitcast(ctx.U32[1], product));
    } else {
        const auto merge = ctx.OpLabel();
        const auto invalid = ctx.OpLabel();
        std::array<Id, 16> labels;
        std::array<Sirit::Literal, 16> values;
        for (u32 i = 0; i < 16; ++i) { labels[i] = ctx.OpLabel(); values[i] = i; }
        const auto operation = load(0);
        ctx.OpSelectionMerge(merge, spv::SelectionControlMask::MaskNone);
        ctx.OpSwitch(operation, invalid, values, labels);
        for (u32 op = 0; op < 16; ++op) {
            ctx.AddLabel(labels[op]);
            Id result{}, bits{};
            switch (op) {
            case 0: result = EmitFPAdd32(ctx, nullptr, a, b); break;
            case 1: result = EmitFPSub32(ctx, nullptr, a, b); break;
            case 2: result = EmitFPMul32(ctx, nullptr, a, b); break;
            case 3: result = EmitFPFma32(ctx, nullptr, a, b, c); break;
            case 4: result = EmitFPMin32(ctx, a, b); break;
            case 5: result = EmitFPMax32(ctx, a, b); break;
            case 6: bits = ctx.OpBitcast(ctx.U32[1], EmitConvertS32F32(ctx, a)); break;
            case 7: result = EmitConvertF32S32(ctx, ctx.OpBitcast(ctx.S32[1], a_bits)); break;
            case 8: bits = EmitPackHalf2x16(ctx, nullptr, ctx.OpCompositeConstruct(ctx.F32[2], a, b)); break;
            case 9: result = ctx.OpCompositeExtract(ctx.F32[1], EmitUnpackHalf2x16(ctx, a_bits), 0u); break;
            case 10: {
                const auto eq = ctx.OpSelect(ctx.U32[1], EmitFPOrdEqual32(ctx, a, b), ctx.ConstU32(1u), ctx.u32_zero_value);
                const auto lt = ctx.OpSelect(ctx.U32[1], EmitFPOrdLessThan32(ctx, a, b), ctx.ConstU32(2u), ctx.u32_zero_value);
                const auto gt = ctx.OpSelect(ctx.U32[1], EmitFPOrdGreaterThan32(ctx, a, b), ctx.ConstU32(4u), ctx.u32_zero_value);
                bits = ctx.OpBitwiseOr(ctx.U32[1], eq, ctx.OpBitwiseOr(ctx.U32[1], lt, gt));
                break;
            }
            case 11: result = EmitFPDiv32(ctx, nullptr, a, b); break;
            case 12: result = EmitFPNeg32(ctx, a); break;
            case 13: {
                const auto product = ctx.OpBitcast(ctx.U32[1], EmitFPMul32(ctx, nullptr, a, b));
                const auto is_zero = ctx.OpIEqual(ctx.U1[1], ctx.OpBitwiseAnd(ctx.U32[1], product, ctx.ConstU32(0x7fffffffu)), ctx.u32_zero_value);
                bits = ctx.OpSelect(ctx.U32[1], is_zero, sign(), product);
                break;
            }
            case 14: result = EmitFPAdd32(ctx, nullptr, EmitFPMul32(ctx, nullptr, a, b), c); break;
            case 15: result = EmitFPFma32(ctx, nullptr, a, b, ctx.OpBitcast(ctx.F32[1], sign())); break;
            }
            store(result.value ? ctx.OpBitcast(ctx.U32[1], result) : bits);
            ctx.OpBranch(merge);
        }
        ctx.AddLabel(invalid);
        store(ctx.ConstU32(0xdeaddeadu));
        ctx.OpBranch(merge);
        ctx.AddLabel(merge);
    }
    ctx.OpReturn();
    ctx.OpFunctionEnd();
    const std::array interfaces{gid, push};
    ctx.AddEntryPoint(spv::ExecutionModel::GLCompute, function, "main", interfaces);
    ctx.AddExecutionMode(function, spv::ExecutionMode::LocalSize, benchmark ? 64u : 1u, 1u, 1u);
    ctx.AddExecutionMode(function, spv::ExecutionMode::SignedZeroInfNanPreserve, 32u);
    return ctx.Assemble();
}
int main(int argc, char** argv) {
    assert(argc == 2);
    for (bool repair : {false, true}) for (bool benchmark : {false, true}) {
        const auto code = Emit(repair, benchmark);
        const auto name = std::string(argv[1]) + (repair ? "/production" : "/baseline") +
                          (benchmark ? "-bench.spv" : ".spv");
        std::ofstream out(name, std::ios::binary);
        out.write(reinterpret_cast<const char*>(code.data()), code.size() * sizeof(u32));
        assert(out);
    }
    std::puts("Production arithmetic shaders emitted");
}
