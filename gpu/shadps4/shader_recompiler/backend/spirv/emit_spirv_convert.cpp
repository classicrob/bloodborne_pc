// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"

namespace Shader::Backend::SPIRV {
namespace {
Id ExtractU16(EmitContext& ctx, Id value) {
    return ctx.OpUConvert(ctx.U16, value);
}

Id ExtractS16(EmitContext& ctx, Id value) {
    return ctx.OpSConvert(ctx.S16, value);
}

Id ExtractU8(EmitContext& ctx, Id value) {
    return ctx.OpUConvert(ctx.U8, value);
}

Id ExtractS8(EmitContext& ctx, Id value) {
    return ctx.OpSConvert(ctx.S8, value);
}

#ifdef __APPLE__
Id FlushHalfSubnormal(EmitContext& ctx, Id value) {
    const Id bits = ctx.OpBitcast(ctx.U16, value);
    const Id magnitude = ctx.OpBitwiseAnd(ctx.U16, bits, ctx.Constant(ctx.U16, u16{0x7fff}));
    const Id subnormal = ctx.OpULessThan(ctx.U1[1], magnitude, ctx.Constant(ctx.U16, u16{0x0400}));
    const Id sign = ctx.OpBitwiseAnd(ctx.U16, bits, ctx.Constant(ctx.U16, u16{0x8000}));
    return ctx.OpBitcast(ctx.F16[1], ctx.OpSelect(ctx.U16, subnormal, sign, bits));
}

void RequireHalfConversion(EmitContext& ctx, const char* opcode) {
    ASSERT_MSG(ctx.profile.support_float16 && ctx.profile.support_fp16_signed_zero_inf_nan_preserve,
               "macOS {} requires native Float16 and signed-zero/Inf/NaN preservation: shader={:#x}",
               opcode, ctx.info.pgm_hash);
}
#endif
} // Anonymous namespace

Id EmitConvertS16F32(EmitContext& ctx, Id value) {
    return ctx.OpSConvert(ctx.U32[1], ctx.OpConvertFToS(ctx.U16, value));
}

Id EmitConvertS16F64(EmitContext& ctx, Id value) {
    return ctx.OpSConvert(ctx.U32[1], ctx.OpConvertFToS(ctx.U16, value));
}

Id EmitConvertS32F32(EmitContext& ctx, Id value) {
    return ctx.OpConvertFToS(ctx.U32[1], value);
}

Id EmitConvertS32F64(EmitContext& ctx, Id value) {
    return ctx.OpConvertFToS(ctx.U32[1], value);
}

Id EmitConvertS64F32(EmitContext& ctx, Id value) {
    return ctx.OpConvertFToS(ctx.U64, value);
}

Id EmitConvertS64F64(EmitContext& ctx, Id value) {
    return ctx.OpConvertFToS(ctx.U64, value);
}

Id EmitConvertU16F32(EmitContext& ctx, Id value) {
    return ctx.OpUConvert(ctx.U32[1], ctx.OpConvertFToU(ctx.U16, value));
}

Id EmitConvertU16F64(EmitContext& ctx, Id value) {
    return ctx.OpUConvert(ctx.U32[1], ctx.OpConvertFToU(ctx.U16, value));
}

Id EmitConvertU32F32(EmitContext& ctx, Id value) {
    return ctx.OpConvertFToU(ctx.U32[1], value);
}

Id EmitConvertU32F64(EmitContext& ctx, Id value) {
    return ctx.OpConvertFToU(ctx.U32[1], value);
}

Id EmitConvertU64F32(EmitContext& ctx, Id value) {
    return ctx.OpConvertFToU(ctx.U64, value);
}

Id EmitConvertU64F64(EmitContext& ctx, Id value) {
    return ctx.OpConvertFToU(ctx.U64, value);
}

Id EmitConvertU64U32(EmitContext& ctx, Id value) {
    return ctx.OpUConvert(ctx.U64, value);
}

Id EmitConvertU32U64(EmitContext& ctx, Id value) {
    return ctx.OpUConvert(ctx.U32[1], value);
}

Id EmitConvertF16F32(EmitContext& ctx, Id value) {
#ifdef __APPLE__
    RequireHalfConversion(ctx, "V_CVT_F16_F32");
    ASSERT_MSG(ctx.runtime_info.props.fp_round_mode16_64 == AmdGpu::FpRoundMode::NearestEven,
               "macOS V_CVT_F16_F32 rounding mode is not implemented: shader={:#x} round16_64={} denorm16_64={}",
               ctx.info.pgm_hash, u32(ctx.runtime_info.props.fp_round_mode16_64),
               u32(ctx.runtime_info.props.fp_denorm_mode16_64));
    const auto mode = ctx.runtime_info.props.fp_denorm_mode16_64;
    const Id converted = ctx.OpFConvert(ctx.F16[1], value);
    if (mode == AmdGpu::FpDenormMode::InOutFlush || mode == AmdGpu::FpDenormMode::InAllowOutFlush) {
        return FlushHalfSubnormal(ctx, converted);
    }
    return converted;
#else
    return ctx.OpFConvert(ctx.F16[1], value);
#endif
}

Id EmitConvertF32F16(EmitContext& ctx, Id value) {
#ifdef __APPLE__
    RequireHalfConversion(ctx, "V_CVT_F32_F16");
    const auto mode = ctx.runtime_info.props.fp_denorm_mode16_64;
    if (mode == AmdGpu::FpDenormMode::InOutFlush || mode == AmdGpu::FpDenormMode::InFlushOutAllow) {
        value = FlushHalfSubnormal(ctx, value);
    }
#endif
    return ctx.OpFConvert(ctx.F32[1], value);
}

Id EmitConvertF32F64(EmitContext& ctx, Id value) {
    return ctx.OpFConvert(ctx.F32[1], value);
}

Id EmitConvertF64F32(EmitContext& ctx, Id value) {
    return ctx.OpFConvert(ctx.F64[1], value);
}

Id EmitConvertF32S8(EmitContext& ctx, Id value) {
    return ctx.OpConvertSToF(ctx.F32[1], ExtractS8(ctx, value));
}

Id EmitConvertF32S16(EmitContext& ctx, Id value) {
    return ctx.OpConvertSToF(ctx.F32[1], ExtractS16(ctx, value));
}

Id EmitConvertF32S32(EmitContext& ctx, Id value) {
    return ctx.OpConvertSToF(ctx.F32[1], value);
}

Id EmitConvertF32S64(EmitContext& ctx, Id value) {
    return ctx.OpConvertSToF(ctx.F32[1], value);
}

Id EmitConvertF32U8(EmitContext& ctx, Id value) {
    return ctx.OpConvertUToF(ctx.F32[1], ExtractU8(ctx, value));
}

Id EmitConvertF32U16(EmitContext& ctx, Id value) {
    return ctx.OpConvertUToF(ctx.F32[1], ExtractU16(ctx, value));
}

Id EmitConvertF32U32(EmitContext& ctx, Id value) {
    return ctx.OpConvertUToF(ctx.F32[1], value);
}

Id EmitConvertF32U64(EmitContext& ctx, Id value) {
    return ctx.OpConvertUToF(ctx.F32[1], value);
}

Id EmitConvertF64S8(EmitContext& ctx, Id value) {
    return ctx.OpConvertSToF(ctx.F64[1], ExtractS8(ctx, value));
}

Id EmitConvertF64S16(EmitContext& ctx, Id value) {
    return ctx.OpConvertSToF(ctx.F64[1], ExtractS16(ctx, value));
}

Id EmitConvertF64S32(EmitContext& ctx, Id value) {
    return ctx.OpConvertSToF(ctx.F64[1], value);
}

Id EmitConvertF64S64(EmitContext& ctx, Id value) {
    return ctx.OpConvertSToF(ctx.F64[1], value);
}

Id EmitConvertF64U8(EmitContext& ctx, Id value) {
    return ctx.OpConvertUToF(ctx.F64[1], ExtractU8(ctx, value));
}

Id EmitConvertF64U16(EmitContext& ctx, Id value) {
    return ctx.OpConvertUToF(ctx.F64[1], ExtractU16(ctx, value));
}

Id EmitConvertF64U32(EmitContext& ctx, Id value) {
    return ctx.OpConvertUToF(ctx.F64[1], value);
}

Id EmitConvertF64U64(EmitContext& ctx, Id value) {
    return ctx.OpConvertUToF(ctx.F64[1], value);
}

Id EmitConvertU16U32(EmitContext& ctx, Id value) {
    return ctx.OpUConvert(ctx.U16, value);
}

Id EmitConvertU32U16(EmitContext& ctx, Id value) {
    return ctx.OpUConvert(ctx.U32[1], value);
}

Id EmitConvertU8U32(EmitContext& ctx, Id value) {
    return ctx.OpUConvert(ctx.U8, value);
}

Id EmitConvertU32U8(EmitContext& ctx, Id value) {
    return ctx.OpUConvert(ctx.U32[1], value);
}

Id EmitConvertS32S8(EmitContext& ctx, Id value) {
    return ctx.OpSConvert(ctx.U32[1], value);
}

Id EmitConvertS32S16(EmitContext& ctx, Id value) {
    return ctx.OpSConvert(ctx.U32[1], value);
}

} // namespace Shader::Backend::SPIRV
