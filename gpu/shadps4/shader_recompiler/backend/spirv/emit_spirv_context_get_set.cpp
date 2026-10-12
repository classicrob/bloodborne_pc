// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/assert.h"
#include "core/emulator_settings.h"
#include "shader_recompiler/backend/spirv/emit_spirv_gs_emulation.h"
#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"
#include "shader_recompiler/ir/attribute.h"
#include "shader_recompiler/ir/microinstruction.h"
#include "shader_recompiler/ir/patch.h"
#include "shader_recompiler/runtime_info.h"

#include <magic_enum/magic_enum.hpp>

namespace Shader::Backend::SPIRV {

using PointerType = EmitContext::PointerType;
using PointerSize = EmitContext::PointerSize;

static std::pair<Id, bool> OutputAttrComponentType(EmitContext& ctx, IR::Attribute attr) {
    if (IR::IsParam(attr)) {
        const u32 index{u32(attr) - u32(IR::Attribute::Param0)};
        const auto& info{ctx.output_params.at(index)};
        return {info.component_type, info.is_integer};
    }
    if (IR::IsMrt(attr)) {
        const u32 index{u32(attr) - u32(IR::Attribute::RenderTarget0)};
        const auto& info{ctx.frag_outputs.at(index)};
        return {info.component_type, info.is_integer};
    }
    switch (attr) {
    case IR::Attribute::Position0:
    case IR::Attribute::ClipDistance:
    case IR::Attribute::CullDistance:
    case IR::Attribute::Depth:
    case IR::Attribute::PointSize:
        return {ctx.F32[1], false};
    case IR::Attribute::RenderTargetIndex:
    case IR::Attribute::ViewportIndex:
    case IR::Attribute::SampleMask:
        return {ctx.U32[1], true};
    case IR::Attribute::StencilRef:
        return {ctx.S32[1], true};
    default:
        UNREACHABLE_MSG("Write attribute {}", attr);
    }
}

Id EmitGetUserData(EmitContext& ctx, IR::ScalarReg reg) {
    const u32 index = ctx.binding.user_data + ctx.info.ud_mask.Index(reg);
    const u32 half = PushData::UdRegsIndex + (index >> 2);
    const Id ud_ptr{ctx.OpAccessChain(ctx.TypePointer(spv::StorageClass::PushConstant, ctx.U32[1]),
                                      ctx.push_data_block, ctx.ConstU32(half),
                                      ctx.ConstU32(index & 3))};
    const Id ud_reg{ctx.OpLoad(ctx.U32[1], ud_ptr)};
    ctx.Name(ud_reg, fmt::format("ud_{}", u32(reg)));
    return ud_reg;
}

Id EmitReadConst(EmitContext& ctx, IR::Inst* inst, Id addr, Id offset) {
    const u32 flatbuf_off_dw = inst->Flags<u32>();
    if (!EmulatorSettings.IsDirectMemoryAccessEnabled()) {
        return ctx.EmitFlatbufferLoad(ctx.ConstU32(flatbuf_off_dw));
    }
    if (flatbuf_off_dw == 0) {
        return ctx.OpFunctionCall(ctx.U32[1], ctx.read_const_dynamic, addr, offset);
    } else {
        return ctx.EmitFlatbufferLoad(ctx.ConstU32(flatbuf_off_dw));
    }
}

Id EmitBufferAddress(EmitContext& ctx, IR::Inst* inst, u32 handle, Id index, Id offset, Id soffset) {
    const auto flags = inst->Flags<IR::BufferInstInfo>();
    const auto& desc = ctx.info.buffers[handle];
    const auto sharp = desc.GetSharp(ctx.info);
    // MUBUF does not replace an INVALID resource format with its implied format.
    Id valid = sharp.GetDataFmt() == AmdGpu::DataFormat::FormatInvalid &&
                       flags.inst_data_fmt == AmdGpu::DataFormat::FormatInvalid
                   ? ctx.false_value : ctx.true_value;
    auto checked_add = [&](Id a, Id b) {
        const Id sum = ctx.OpIAdd(ctx.U32[1], a, b);
        valid = ctx.OpLogicalAnd(ctx.U1[1], valid, ctx.OpUGreaterThanEqual(ctx.U1[1], sum, a));
        return sum;
    };
    auto checked_mul = [&](Id a, u32 factor) {
        if (factor == 0) return ctx.u32_zero_value;
        valid = ctx.OpLogicalAnd(ctx.U1[1], valid,
            ctx.OpULessThanEqual(ctx.U1[1], a, ctx.ConstU32(UINT32_MAX / factor)));
        return ctx.OpIMul(ctx.U32[1], a, ctx.ConstU32(factor));
    };
    if (!flags.index_enable) index = ctx.u32_zero_value;
    if (sharp.stride != 0 && sharp.add_tid_enable) {
        // GCN thread_id[5:0] is the lane in the 64-wide wave. On Apple (32-wide subgroups,
        // emulate_wave64) that is LocalInvocationIndex & 63; add_tid exists only in compute.
        const Id thread_id =
            ctx.profile.emulate_wave64
                ? ctx.OpBitwiseAnd(ctx.U32[1], ctx.OpLoad(ctx.U32[1], ctx.local_invocation_index),
                                   ctx.ConstU32(63u))
                : ctx.OpLoad(ctx.U32[1], ctx.subgroup_local_invocation_id);
        index = checked_add(index, thread_id);
    }
    if (!flags.voffset_enable) offset = ctx.u32_zero_value;
    offset = checked_add(offset, ctx.ConstU32(u32(flags.inst_offset.Value())));
    Id address;
    if (sharp.swizzle_enable) {
        const u32 index_stride = sharp.GetIndexStride();
        const u32 element = sharp.GetElementSize();
        const Id index_msb = ctx.OpUDiv(ctx.U32[1], index, ctx.ConstU32(index_stride));
        const Id index_lsb = ctx.OpUMod(ctx.U32[1], index, ctx.ConstU32(index_stride));
        const Id offset_msb = ctx.OpUDiv(ctx.U32[1], offset, ctx.ConstU32(element));
        const Id offset_lsb = ctx.OpUMod(ctx.U32[1], offset, ctx.ConstU32(element));
        const Id msb = checked_mul(checked_add(checked_mul(index_msb, sharp.stride),
                                              checked_mul(offset_msb, element)), index_stride);
        address = checked_add(msb, checked_add(checked_mul(index_lsb, element), offset_lsb));
    } else {
        address = checked_add(checked_mul(index, sharp.stride), offset);
    }
    // GCN applies the SGPR offset to the base, after the swizzle calculation.
    address = checked_add(address, soffset);
    const Id records = ctx.BufferNumRecords(handle);
    const bool formatted = flags.typed.Value();
    const u32 bytes = flags.access_bytes.Value();
    std::array<Id, 4> predicates;
    for (u32 c = 0; c < predicates.size(); ++c) {
        const u32 delta = formatted ? 0u : c * 4;
        Id predicate = valid;
        if (sharp.stride == 0) {
            const Id end = ctx.OpIAdd(ctx.U32[1], address, ctx.ConstU32(delta));
            predicate = ctx.OpLogicalAnd(ctx.U1[1], predicate,
                ctx.OpUGreaterThanEqual(ctx.U1[1], end, address));
            predicate = ctx.OpLogicalAnd(ctx.U1[1], predicate,
                ctx.OpULessThan(ctx.U1[1], end, records));
        } else {
            predicate = ctx.OpLogicalAnd(ctx.U1[1], predicate,
                ctx.OpULessThan(ctx.U1[1], index, records));
            if (flags.index_enable || sharp.add_tid_enable) {
                const Id end = ctx.OpIAdd(ctx.U32[1], offset, ctx.ConstU32(delta));
                predicate = ctx.OpLogicalAnd(ctx.U1[1], predicate,
                    ctx.OpUGreaterThanEqual(ctx.U1[1], end, offset));
                predicate = ctx.OpLogicalAnd(ctx.U1[1], predicate,
                    ctx.OpULessThan(ctx.U1[1], end, ctx.ConstU32(u32(sharp.stride))));
            }
        }
        predicates[c] = predicate;
    }
    // Alignment applies to the complete byte address, including the binding prefix.
    const u32 shift = flags.address_shift.Value();
    const Id prefix = ctx.buffers[handle].Offset(PointerSize::B8);
    if (Sirit::ValidId(prefix)) {
        const Id sum = ctx.OpIAdd(ctx.U32[1], address,
            ctx.OpBitwiseAnd(ctx.U32[1], prefix, ctx.ConstU32((1u << shift) - 1)));
        const Id no_carry = ctx.OpUGreaterThanEqual(ctx.U1[1], sum, address);
        for (Id& predicate : predicates)
            predicate = ctx.OpLogicalAnd(ctx.U1[1], predicate, no_carry);
        address = sum;
    }
    const Id aligned = ctx.OpShiftRightLogical(ctx.U32[1], address, ctx.ConstU32(shift));
    if (formatted) {
        // A formatted instruction is one element: a partial host range drops the whole access,
        // including component swizzles that would otherwise turn a zero load into constant ONE.
        const auto alias = shift == 0 ? PointerType::U8 : shift == 1 ? PointerType::U16 : PointerType::U32;
        const Id aligned_prefix = Sirit::ValidId(prefix)
            ? ctx.OpShiftRightLogical(ctx.U32[1], prefix, ctx.ConstU32(shift)) : ctx.u32_zero_value;
        const Id first = ctx.OpIAdd(ctx.U32[1], aligned, aligned_prefix);
        const Id last = ctx.OpIAdd(ctx.U32[1], first, ctx.ConstU32((bytes >> shift) - 1));
        Id complete = ctx.OpUGreaterThanEqual(ctx.U1[1], first, aligned);
        complete = ctx.OpLogicalAnd(ctx.U1[1], complete, ctx.OpUGreaterThanEqual(ctx.U1[1], last, first));
        complete = ctx.OpLogicalAnd(ctx.U1[1], complete,
            ctx.OpULessThan(ctx.U1[1], last, ctx.BufferLength(handle, alias)));
        for (Id& predicate : predicates)
            predicate = ctx.OpLogicalAnd(ctx.U1[1], predicate, complete);
    }
    for (Id& predicate : predicates)
        predicate = GsEmulationGuardAccess(ctx, predicate);
    const Id result = ctx.OpSelect(ctx.U32[1], predicates[0], aligned, ctx.ConstU32(UINT32_MAX));
    ctx.buffer_address_bounds.emplace(result.value, EmitContext::BufferAddressBounds{predicates, bytes, formatted});
    return result;
}

Id EmitReadConstBuffer(EmitContext& ctx, u32 handle, Id index, Id previous) {
    const Id original_index = index;
    const auto& buffer = ctx.buffers[handle];
    if (const Id offset = buffer.Offset(PointerSize::B32); Sirit::ValidId(offset)) {
        index = ctx.OpIAdd(ctx.U32[1], index, offset);
    }
    const auto [id, pointer_type] = buffer.Alias(PointerType::U32);
    const auto read = [&] {
        return ctx.OpLoad(ctx.U32[1],
            ctx.OpAccessChain(pointer_type, id, ctx.u32_zero_value, index));
    };
    if (!ctx.profile.emulate_robust_buffer_access || buffer.buffer_type != BufferType::Guest) {
        return ctx.BufferRead(handle, PointerType::U32, original_index, index, ctx.U32[1], read);
    }
    if (buffer.is_null) return ctx.u32_zero_value;
    const auto& desc = ctx.info.buffers[handle];
    const auto sharp = desc.GetSharp(ctx.info);
    const Id records = ctx.BufferNumRecords(handle);
    // SMRD clamps each DWORD separately and leaves a clamped destination SGPR unchanged.
    // Its bound uses only base, stride and num_records, including INVALID formats.
    const Id bytes = ctx.OpShiftLeftLogical(ctx.U32[1], original_index, ctx.ConstU32(2u));
    Id valid = ctx.OpULessThanEqual(ctx.U1[1], original_index, ctx.ConstU32(UINT32_MAX / 4));
    const Id bound = sharp.stride == 0 ? records
        : ctx.OpIMul(ctx.U32[1], records, ctx.ConstU32(u32(sharp.stride)));
    const Id guest_valid = sharp.stride == 0
        ? ctx.OpULessThan(ctx.U1[1], bytes, bound)
        : ctx.OpULessThanEqual(ctx.U1[1], bytes, bound);
    valid = ctx.OpLogicalAnd(ctx.U1[1], valid, guest_valid);
    valid = ctx.OpLogicalAnd(ctx.U1[1], valid,
        ctx.BufferInBounds(handle, PointerType::U32, original_index, index, 0));
    const Id access_label = ctx.OpLabel();
    const Id clamp_label = ctx.OpLabel();
    const Id merge_label = ctx.OpLabel();
    ctx.OpSelectionMerge(merge_label, spv::SelectionControlMask::MaskNone);
    ctx.OpBranchConditional(valid, access_label, clamp_label);
    ctx.AddLabel(access_label);
    const Id value = read();
    ctx.OpBranch(merge_label);
    ctx.AddLabel(clamp_label);
    ctx.OpBranch(merge_label);
    ctx.AddLabel(merge_label);
    return ctx.OpPhi(ctx.U32[1], value, access_label, previous, clamp_label);
}

Id EmitGetAttribute(EmitContext& ctx, IR::Attribute attr, u32 comp, u32 index) {
    if (const auto value = GsEmulationGetAttribute(ctx, attr, comp, index)) {
        return *value;
    }
    if (IR::IsParam(attr)) {
        const u32 param_index{u32(attr) - u32(IR::Attribute::Param0)};
        const auto& param{ctx.input_params.at(param_index)};
        if (param.is_null) return ctx.f32_zero_value;
        const Id value = [&] {
            if (param.is_array) {
                ASSERT(param.num_components > 1);
                if (param.is_loaded) {
                    return ctx.OpCompositeExtract(param.component_type, param.id_array[index],
                                                  comp);
                } else {
                    return ctx.OpLoad(param.component_type,
                                      ctx.OpAccessChain(param.pointer_type, param.id,
                                                        ctx.ConstU32(index), ctx.ConstU32(comp)));
                }
            } else {
                ASSERT(!param.is_loaded);
                if (param.num_components > 1) {
                    return ctx.OpLoad(
                        param.component_type,
                        ctx.OpAccessChain(param.pointer_type, param.id, ctx.ConstU32(comp)));
                } else {
                    return ctx.OpLoad(param.component_type, param.id);
                }
            }
        }();
        return param.is_integer ? ctx.OpBitcast(ctx.F32[1], value) : value;
    }
    if (IR::IsBarycentricCoord(attr) && attr != IR::Attribute::BaryCoordPullModel &&
        ctx.profile.supports_fragment_shader_barycentric) {
        ++comp;
    }
    switch (attr) {
    case IR::Attribute::Position0:
        ASSERT(ctx.sw_stage == SwStage::Geometry);
        return ctx.OpLoad(ctx.F32[1],
                          ctx.OpAccessChain(ctx.input_f32, ctx.gl_in, ctx.ConstU32(index),
                                            ctx.ConstU32(0U), ctx.ConstU32(comp)));
    case IR::Attribute::FragCoord: {
        const Id value = ctx.OpLoad(ctx.F32[1],
            ctx.OpAccessChain(ctx.input_f32, ctx.frag_coord, ctx.ConstU32(comp)));
        if (comp >= 2) return value;
        // Guest shaders still address native-sized textures and constants. Convert the
        // reduced framebuffer's pixel coordinates back to that logical coordinate space.
        const Id packed = ctx.OpLoad(ctx.U32[1], ctx.OpAccessChain(
            ctx.TypePointer(spv::StorageClass::PushConstant, ctx.U32[1]),
            ctx.push_data_block, ctx.ConstU32(PushData::SceneSizeIndex)));
        const Id dimension = comp == 0
            ? ctx.OpBitwiseAnd(ctx.U32[1], packed, ctx.ConstU32(65535u))
            : ctx.OpShiftRightLogical(ctx.U32[1], packed, ctx.ConstU32(16u));
        const Id native = ctx.ConstU32(comp == 0 ? 1920u : 1080u);
        const Id safe = ctx.OpSelect(ctx.U32[1],
            ctx.OpIEqual(ctx.U1[1], dimension, ctx.u32_zero_value), native, dimension);
        return ctx.OpFMul(ctx.F32[1], value, ctx.OpFDiv(ctx.F32[1],
            ctx.OpConvertUToF(ctx.F32[1], native), ctx.OpConvertUToF(ctx.F32[1], safe)));
    }
    case IR::Attribute::TessellationEvaluationPointU:
        return ctx.OpLoad(ctx.F32[1],
                          ctx.OpAccessChain(ctx.input_f32, ctx.tess_coord, ctx.u32_zero_value));
    case IR::Attribute::TessellationEvaluationPointV:
        return ctx.OpLoad(ctx.F32[1],
                          ctx.OpAccessChain(ctx.input_f32, ctx.tess_coord, ctx.ConstU32(1U)));
    case IR::Attribute::BaryCoordSmooth:
        if (ctx.profile.supports_amd_shader_explicit_vertex_parameter) {
            return ctx.OpLoad(ctx.F32[1], ctx.OpAccessChain(ctx.input_f32, ctx.bary_coord_smooth,
                                                            ctx.ConstU32(comp)));
        } else {
            return ctx.OpCompositeExtract(ctx.F32[1], ctx.OpLoad(ctx.F32[3], ctx.bary_coord), comp);
        }
    case IR::Attribute::BaryCoordSmoothCentroid:
        if (ctx.profile.supports_amd_shader_explicit_vertex_parameter) {
            return ctx.OpLoad(ctx.F32[1],
                              ctx.OpAccessChain(ctx.input_f32, ctx.bary_coord_smooth_centroid,
                                                ctx.ConstU32(comp)));
        } else {
            return ctx.OpCompositeExtract(
                ctx.F32[1], ctx.OpInterpolateAtCentroid(ctx.F32[3], ctx.bary_coord), comp);
        }
    case IR::Attribute::BaryCoordSmoothSample:
        if (ctx.profile.supports_amd_shader_explicit_vertex_parameter) {
            return ctx.OpLoad(
                ctx.F32[1],
                ctx.OpAccessChain(ctx.input_f32, ctx.bary_coord_smooth_sample, ctx.ConstU32(comp)));
        } else {
            return ctx.OpCompositeExtract(
                ctx.F32[1],
                ctx.OpInterpolateAtSample(ctx.F32[3], ctx.bary_coord,
                                          ctx.OpLoad(ctx.U32[1], ctx.sample_index)),
                comp);
        }
    case IR::Attribute::BaryCoordNoPersp:
        return ctx.OpLoad(ctx.F32[1], ctx.OpAccessChain(ctx.input_f32, ctx.bary_coord_nopersp,
                                                        ctx.ConstU32(comp)));
    case IR::Attribute::BaryCoordNoPerspSample:
        return ctx.OpLoad(
            ctx.F32[1],
            ctx.OpAccessChain(ctx.input_f32, ctx.bary_coord_nopersp_sample, ctx.ConstU32(comp)));
    case IR::Attribute::BaryCoordPullModel: {
        ASSERT_MSG(comp < 3, "Invalid BaryCoordPullModel component {}", comp);

        if (ctx.profile.supports_amd_shader_explicit_vertex_parameter) {
            return ctx.OpLoad(
                ctx.F32[1],
                ctx.OpAccessChain(ctx.input_f32, ctx.bary_coord_pull_model, ctx.ConstU32(comp)));
        }

        // FragCoord.w is reciprocal clip W at the fragment center. Perspective barycentrics
        // are (I/W)/(1/W) and (J/W)/(1/W), so multiplying by FragCoord.w recovers the
        // pull-model values expected by the guest.
        const Id inv_w = ctx.OpLoad(
            ctx.F32[1], ctx.OpAccessChain(ctx.input_f32, ctx.frag_coord, ctx.ConstU32(3U)));
        if (comp == 2) {
            return inv_w;
        }
        const Id ij =
            ctx.OpCompositeExtract(ctx.F32[1], ctx.OpLoad(ctx.F32[3], ctx.bary_coord), comp + 1);
        return ctx.OpFMul(ctx.F32[1], ij, inv_w);
    }
    default:
        UNREACHABLE_MSG("Read attribute {}", attr);
    }
}

Id EmitGetAttributeU1(EmitContext& ctx, IR::Attribute attr, u32 comp) {
    ASSERT(comp == 0);
    if (const auto value = GsEmulationGetAttributeU1(ctx, attr)) {
        return *value;
    }
    switch (attr) {
    case IR::Attribute::IsFrontFace:
        return ctx.OpLoad(ctx.U1[1], ctx.front_facing);
    case IR::Attribute::IsHelperInvocation:
        return ctx.OpLoad(ctx.U1[1], ctx.helper_invocation);
    default:
        UNREACHABLE_MSG("Unsupported U1 attribute {}", attr);
    }
}

Id EmitGetAttributeU32(EmitContext& ctx, IR::Attribute attr, u32 comp) {
    if (const auto value = GsEmulationGetAttributeU32(ctx, attr)) {
        return *value;
    }
    switch (attr) {
    case IR::Attribute::VertexId:
        return ctx.OpLoad(ctx.U32[1], ctx.vertex_index);
    case IR::Attribute::InstanceId:
        return ctx.OpLoad(ctx.U32[1], ctx.instance_id);
    case IR::Attribute::BaseVertex:
        return ctx.OpLoad(ctx.U32[1], ctx.base_vertex);
    case IR::Attribute::BaseInstance:
        return ctx.OpLoad(ctx.U32[1], ctx.base_instance);
    case IR::Attribute::WorkgroupIndex:
        return ctx.workgroup_index_id;
    case IR::Attribute::WorkgroupId:
        return ctx.OpCompositeExtract(ctx.U32[1], ctx.OpLoad(ctx.U32[3], ctx.workgroup_id), comp);
    case IR::Attribute::LocalInvocationId:
        return ctx.OpCompositeExtract(ctx.U32[1], ctx.OpLoad(ctx.U32[3], ctx.local_invocation_id),
                                      comp);
    case IR::Attribute::LocalInvocationIndex:
        return ctx.OpLoad(ctx.U32[1], ctx.local_invocation_index);
    case IR::Attribute::SampleIndex:
        return ctx.OpLoad(ctx.U32[1], ctx.sample_index);
    case IR::Attribute::SampleMask:
        return ctx.OpLoad(ctx.U32[1],
                          ctx.OpAccessChain(ctx.input_u32, ctx.sample_mask_in, ctx.u32_zero_value));
    case IR::Attribute::RenderTargetIndex:
        return ctx.OpLoad(ctx.U32[1], ctx.output_layer);
    case IR::Attribute::PrimitiveId:
        return ctx.OpLoad(ctx.U32[1], ctx.primitive_id);
    case IR::Attribute::InvocationId:
        ASSERT(ctx.info.sw_stage == SwStage::Geometry ||
               ctx.info.sw_stage == SwStage::TessellationControl);
        return ctx.OpLoad(ctx.U32[1], ctx.invocation_id);
    case IR::Attribute::SubgroupLtMask:
        return ctx.OpLoad(
            ctx.U32[1], ctx.OpAccessChain(ctx.input_u32, ctx.subgroup_lt_mask, ctx.ConstU32(comp)));
    case IR::Attribute::PatchVertices:
        ASSERT(ctx.info.sw_stage == SwStage::TessellationControl);
        return ctx.OpLoad(ctx.U32[1], ctx.patch_vertices);
    case IR::Attribute::PackedHullInvocationInfo: {
        ASSERT(ctx.info.sw_stage == SwStage::TessellationControl);
        // [0:8]: patch id within VGT
        // [8:12]: output control point id
        // But 0:8 should be treated as 0 for attribute addressing purposes
        if (ctx.runtime_info.sw.tcs.IsPassthrough()) {
            // Gcn shader would run with 1 thread, but we need to run a thread for
            // each output control point.
            // If Gcn shader uses this value, we should make sure all threads in the
            // Vulkan shader use 0
            return ctx.u32_zero_value;
        } else {
            const Id invocation_id = ctx.OpLoad(ctx.U32[1], ctx.invocation_id);
            return ctx.OpShiftLeftLogical(ctx.U32[1], invocation_id, ctx.ConstU32(8u));
        }
    }
    default:
        UNREACHABLE_MSG("Read U32 attribute {}", attr);
    }
}

void EmitSetAttribute(EmitContext& ctx, IR::Attribute attr, Id value, u32 element) {
    if (GsEmulationSetAttribute(ctx, attr, value, element)) {
        return;
    }
    const auto op_store = [&](Id pointer) {
        const auto [component_type, is_integer] = OutputAttrComponentType(ctx, attr);
        if (is_integer) {
            ctx.OpStore(pointer, ctx.OpBitcast(component_type, value));
        } else {
            ctx.OpStore(pointer, value);
        }
    };
    if (IR::IsParam(attr)) {
        const u32 attr_index{u32(attr) - u32(IR::Attribute::Param0)};
        if (ctx.hw_stage == HwStage::Local) {
            const auto component_ptr = ctx.TypePointer(spv::StorageClass::Output, ctx.F32[1]);
            return op_store(ctx.OpAccessChain(component_ptr, ctx.output_attr_array,
                                              ctx.ConstU32(attr_index), ctx.ConstU32(element)));
        } else {
            const auto& info{ctx.output_params.at(attr_index)};
            ASSERT(info.num_components > 0);
            if (info.num_components == 1) {
                return op_store(info.id);
            } else {
                return op_store(
                    ctx.OpAccessChain(info.pointer_type, info.id, ctx.ConstU32(element)));
            }
        }
    }
    if (IR::IsMrt(attr)) {
        const u32 index{u32(attr) - u32(IR::Attribute::RenderTarget0)};
        const auto& info{ctx.frag_outputs.at(index)};
        if (element < 3 && ctx.runtime_info.hw.fs.color_buffers[index].blend_self_scale) {
            // Emulates GCN's factor-scaled min/max blend: min/max(src*src, dst*dst).
            value = ctx.OpFMul(ctx.F32[1], value, value);
        }
        if (info.num_components == 1) {
            return op_store(info.id);
        } else {
            return op_store(ctx.OpAccessChain(info.pointer_type, info.id, ctx.ConstU32(element)));
        }
    }
    switch (attr) {
    case IR::Attribute::Position0:
        return op_store(
            ctx.OpAccessChain(ctx.output_f32, ctx.output_position, ctx.ConstU32(element)));
    case IR::Attribute::ClipDistance:
        return op_store(
            ctx.OpAccessChain(ctx.output_f32, ctx.clip_distances, ctx.ConstU32(element)));
    case IR::Attribute::CullDistance:
        return op_store(
            ctx.OpAccessChain(ctx.output_f32, ctx.cull_distances, ctx.ConstU32(element)));
    case IR::Attribute::PointSize:
        return op_store(ctx.output_point_size);
    case IR::Attribute::RenderTargetIndex:
        return op_store(ctx.output_layer);
    case IR::Attribute::ViewportIndex:
        return op_store(ctx.output_viewport_index);
    case IR::Attribute::Depth:
        return op_store(ctx.frag_depth);
    case IR::Attribute::SampleMask:
        return op_store(ctx.OpAccessChain(ctx.output_u32, ctx.sample_mask, ctx.u32_zero_value));
    case IR::Attribute::StencilRef:
        if (ctx.profile.supports_shader_stencil_export) {
            return op_store(ctx.stencil_ref);
        }
        return;
    default:
        UNREACHABLE_MSG("Write attribute {}", attr);
    }
}

Id EmitGetTessGenericAttribute(EmitContext& ctx, Id vertex_index, Id attr_index, Id comp_index) {
    const auto attr_comp_ptr = ctx.TypePointer(spv::StorageClass::Input, ctx.F32[1]);
    return ctx.OpLoad(ctx.F32[1], ctx.OpAccessChain(attr_comp_ptr, ctx.input_attr_array,
                                                    vertex_index, attr_index, comp_index));
}

Id EmitReadTcsGenericOuputAttribute(EmitContext& ctx, Id vertex_index, Id attr_index,
                                    Id comp_index) {
    const auto attr_comp_ptr = ctx.TypePointer(spv::StorageClass::Output, ctx.F32[1]);
    return ctx.OpLoad(ctx.F32[1], ctx.OpAccessChain(attr_comp_ptr, ctx.output_attr_array,
                                                    vertex_index, attr_index, comp_index));
}

void EmitSetTcsGenericAttribute(EmitContext& ctx, Id value, Id attr_index, Id comp_index) {
    // Implied vertex index is invocation_id
    const auto component_ptr = ctx.TypePointer(spv::StorageClass::Output, ctx.F32[1]);
    Id pointer =
        ctx.OpAccessChain(component_ptr, ctx.output_attr_array,
                          ctx.OpLoad(ctx.U32[1], ctx.invocation_id), attr_index, comp_index);
    ctx.OpStore(pointer, value);
}

Id EmitGetPatch(EmitContext& ctx, IR::Patch patch) {
    const u32 index{IR::GenericPatchIndex(patch)};
    const Id element{ctx.ConstU32(IR::GenericPatchElement(patch))};
    const Id type{ctx.sw_stage == SwStage::TessellationControl ? ctx.output_f32 : ctx.input_f32};
    const Id pointer{ctx.OpAccessChain(type, ctx.patches.at(index), element)};
    return ctx.OpLoad(ctx.F32[1], pointer);
}

void EmitSetPatch(EmitContext& ctx, IR::Patch patch, Id value) {
    const Id pointer{[&] {
        if (IR::IsGeneric(patch)) {
            const u32 index{IR::GenericPatchIndex(patch)};
            const Id element{ctx.ConstU32(IR::GenericPatchElement(patch))};
            return ctx.OpAccessChain(ctx.output_f32, ctx.patches.at(index), element);
        }
        switch (patch) {
        case IR::Patch::TessellationLodLeft:
        case IR::Patch::TessellationLodRight:
        case IR::Patch::TessellationLodTop:
        case IR::Patch::TessellationLodBottom: {
            const u32 index{static_cast<u32>(patch) - u32(IR::Patch::TessellationLodLeft)};
            const Id index_id{ctx.ConstU32(index)};
            return ctx.OpAccessChain(ctx.output_f32, ctx.output_tess_level_outer, index_id);
        }
        case IR::Patch::TessellationLodInteriorU:
            return ctx.OpAccessChain(ctx.output_f32, ctx.output_tess_level_inner,
                                     ctx.u32_zero_value);
        case IR::Patch::TessellationLodInteriorV:
            return ctx.OpAccessChain(ctx.output_f32, ctx.output_tess_level_inner, ctx.ConstU32(1u));
        default:
            UNREACHABLE_MSG("Patch {}", u32(patch));
        }
    }()};
    ctx.OpStore(pointer, value);
}

template <u32 N, PointerType alias>
static Id EmitLoadBufferB32xN(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address) {
    const Id original_index = address;
    const auto& spv_buffer = ctx.buffers[handle];
    if (const Id offset = spv_buffer.Offset(PointerSize::B32); Sirit::ValidId(offset)) {
        address = ctx.OpIAdd(ctx.U32[1], address, offset);
    }
    const auto& data_types = alias == PointerType::U32 ? ctx.U32 : ctx.F32;
    const auto [id, pointer_type] = spv_buffer.Alias(alias);

    boost::container::static_vector<Id, N> ids;
    for (u32 i = 0; i < N; i++) {
        const Id index_i = i == 0 ? address : ctx.OpIAdd(ctx.U32[1], address, ctx.ConstU32(i));
        const Id result_i = ctx.BufferLoad(handle, alias, original_index, index_i, data_types[1], [&](Id safe_index) {
            const Id ptr_i = ctx.OpAccessChain(pointer_type, id, ctx.u32_zero_value, safe_index);
            return ctx.OpLoad(data_types[1], ptr_i);
        }, i);
        ids.push_back(result_i);
    }

    const Id result = N == 1 ? ids[0] : ctx.OpCompositeConstruct(data_types[N], ids);
    return result;
}

Id EmitLoadBufferU8(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address) {
    const Id original_index = address;
    const auto& spv_buffer = ctx.buffers[handle];
    if (const Id offset = spv_buffer.Offset(PointerSize::B8); Sirit::ValidId(offset)) {
        address = ctx.OpIAdd(ctx.U32[1], address, offset);
    }
    const auto [id, pointer_type] = spv_buffer.Alias(PointerType::U8);
    return ctx.BufferLoad(handle, PointerType::U8, original_index, address, ctx.U8, [&](Id safe_index) {
        const Id ptr{ctx.OpAccessChain(pointer_type, id, ctx.u32_zero_value, safe_index)};
        return ctx.OpLoad(ctx.U8, ptr);
    });
}

Id EmitLoadBufferU16(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address) {
    const Id original_index = address;
    const auto& spv_buffer = ctx.buffers[handle];
    if (const Id offset = spv_buffer.Offset(PointerSize::B16); Sirit::ValidId(offset)) {
        address = ctx.OpIAdd(ctx.U32[1], address, offset);
    }
    const auto [id, pointer_type] = spv_buffer.Alias(PointerType::U16);
    return ctx.BufferLoad(handle, PointerType::U16, original_index, address, ctx.U16, [&](Id safe_index) {
        const Id ptr{ctx.OpAccessChain(pointer_type, id, ctx.u32_zero_value, safe_index)};
        return ctx.OpLoad(ctx.U16, ptr);
    });
}

Id EmitLoadBufferU32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address) {
    return EmitLoadBufferB32xN<1, PointerType::U32>(ctx, inst, handle, address);
}

Id EmitLoadBufferU32x2(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address) {
    return EmitLoadBufferB32xN<2, PointerType::U32>(ctx, inst, handle, address);
}

Id EmitLoadBufferU32x3(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address) {
    return EmitLoadBufferB32xN<3, PointerType::U32>(ctx, inst, handle, address);
}

Id EmitLoadBufferU32x4(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address) {
    return EmitLoadBufferB32xN<4, PointerType::U32>(ctx, inst, handle, address);
}

Id EmitLoadBufferU64(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address) {
    if (ctx.profile.emulate_robust_buffer_access && ctx.buffer_address_bounds.contains(address.value)) {
        const Id original = address;
        const auto& buffer = ctx.buffers[handle];
        if (const Id offset = buffer.Offset(PointerSize::B32); Sirit::ValidId(offset)) {
            address = ctx.OpIAdd(ctx.U32[1], address, offset);
        }
        const auto [id, pointer_type] = buffer.Alias(PointerType::U32);
        std::array<Id, 2> values;
        for (u32 c = 0; c < values.size(); ++c) {
            const Id index = ctx.OpIAdd(ctx.U32[1], address, ctx.ConstU32(c));
            values[c] = ctx.BufferLoad(handle, PointerType::U32, original, index, ctx.U32[1], [&](Id safe_index) {
                return ctx.OpLoad(ctx.U32[1], ctx.OpAccessChain(pointer_type, id, ctx.u32_zero_value, safe_index));
            }, c);
        }
        return ctx.OpBitcast(ctx.U64, ctx.OpCompositeConstruct(ctx.U32[2], values));
    }
    const Id original_index = address;
    const auto& spv_buffer = ctx.buffers[handle];
    if (const Id offset = spv_buffer.Offset(PointerSize::B64); Sirit::ValidId(offset)) {
        address = ctx.OpIAdd(ctx.U32[1], address, offset);
    }
    const auto [id, pointer_type] = spv_buffer.Alias(PointerType::U64);
    return ctx.BufferLoad(handle, PointerType::U64, original_index, address, ctx.U64, [&](Id safe_index) {
        const Id ptr{ctx.OpAccessChain(pointer_type, id, ctx.u64_zero_value, safe_index)};
        return ctx.OpLoad(ctx.U64, ptr);
    });
}

Id EmitLoadBufferF32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address) {
    return EmitLoadBufferB32xN<1, PointerType::F32>(ctx, inst, handle, address);
}

Id EmitLoadBufferF32x2(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address) {
    return EmitLoadBufferB32xN<2, PointerType::F32>(ctx, inst, handle, address);
}

Id EmitLoadBufferF32x3(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address) {
    return EmitLoadBufferB32xN<3, PointerType::F32>(ctx, inst, handle, address);
}

Id EmitLoadBufferF32x4(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address) {
    return EmitLoadBufferB32xN<4, PointerType::F32>(ctx, inst, handle, address);
}

Id EmitLoadBufferFormatF32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address) {
    UNREACHABLE_MSG("SPIR-V instruction");
}

template <u32 N, PointerType alias>
static void EmitStoreBufferB32xN(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address,
                                 Id value) {
    const Id original_index = address;
    const auto& spv_buffer = ctx.buffers[handle];
    if (const Id offset = spv_buffer.Offset(PointerSize::B32); Sirit::ValidId(offset)) {
        address = ctx.OpIAdd(ctx.U32[1], address, offset);
    }
    const auto& data_types = alias == PointerType::U32 ? ctx.U32 : ctx.F32;
    const auto [id, pointer_type] = spv_buffer.Alias(alias);

    for (u32 i = 0; i < N; i++) {
        const Id index_i = i == 0 ? address : ctx.OpIAdd(ctx.U32[1], address, ctx.ConstU32(i));
        const Id value_i = N == 1 ? value : ctx.OpCompositeExtract(data_types[1], value, i);
        ctx.BufferWrite(handle, alias, original_index, index_i, [&] {
            const Id ptr_i = ctx.OpAccessChain(pointer_type, id, ctx.u32_zero_value, index_i);
            ctx.OpStore(ptr_i, value_i);
        }, i);
    }
}

void EmitStoreBufferU8(EmitContext& ctx, IR::Inst*, u32 handle, Id address, Id value) {
    const Id original_index = address;
    const auto& spv_buffer = ctx.buffers[handle];
    if (const Id offset = spv_buffer.Offset(PointerSize::B8); Sirit::ValidId(offset)) {
        address = ctx.OpIAdd(ctx.U32[1], address, offset);
    }
    const auto [id, pointer_type] = spv_buffer.Alias(PointerType::U8);
    ctx.BufferWrite(handle, PointerType::U8, original_index, address, [&] {
        const Id ptr{ctx.OpAccessChain(pointer_type, id, ctx.u32_zero_value, address)};
        ctx.OpStore(ptr, value);
    });
}

void EmitStoreBufferU16(EmitContext& ctx, IR::Inst*, u32 handle, Id address, Id value) {
    const Id original_index = address;
    const auto& spv_buffer = ctx.buffers[handle];
    if (const Id offset = spv_buffer.Offset(PointerSize::B16); Sirit::ValidId(offset)) {
        address = ctx.OpIAdd(ctx.U32[1], address, offset);
    }
    const auto [id, pointer_type] = spv_buffer.Alias(PointerType::U16);
    ctx.BufferWrite(handle, PointerType::U16, original_index, address, [&] {
        const Id ptr{ctx.OpAccessChain(pointer_type, id, ctx.u32_zero_value, address)};
        ctx.OpStore(ptr, value);
    });
}

void EmitStoreBufferMaskedU32(EmitContext& ctx, u32 handle, Id address, Id value, Id mask) {
    const Id original_index = address;
    const auto& buffer = ctx.buffers[handle];
    if (const Id offset = buffer.Offset(PointerSize::B32); Sirit::ValidId(offset)) {
        address = ctx.OpIAdd(ctx.U32[1], address, offset);
    }
    const auto [id, pointer_type] = buffer.Alias(PointerType::U32);
    ctx.BufferWrite(handle, PointerType::U32, original_index, address, [&] {
        const Id pointer = ctx.OpAccessChain(pointer_type, id, ctx.u32_zero_value, address);
        ctx.WriteMaskedU32(pointer, value, mask, spv::Scope::Device);
    });
}

void EmitStoreBufferU32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address, Id value) {
    EmitStoreBufferB32xN<1, PointerType::U32>(ctx, inst, handle, address, value);
}

void EmitStoreBufferU32x2(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address, Id value) {
    EmitStoreBufferB32xN<2, PointerType::U32>(ctx, inst, handle, address, value);
}

void EmitStoreBufferU32x3(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address, Id value) {
    EmitStoreBufferB32xN<3, PointerType::U32>(ctx, inst, handle, address, value);
}

void EmitStoreBufferU32x4(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address, Id value) {
    EmitStoreBufferB32xN<4, PointerType::U32>(ctx, inst, handle, address, value);
}

void EmitStoreBufferU64(EmitContext& ctx, IR::Inst*, u32 handle, Id address, Id value) {
    if (ctx.profile.emulate_robust_buffer_access && ctx.buffer_address_bounds.contains(address.value)) {
        const Id original = address;
        const auto& buffer = ctx.buffers[handle];
        if (const Id offset = buffer.Offset(PointerSize::B32); Sirit::ValidId(offset)) {
            address = ctx.OpIAdd(ctx.U32[1], address, offset);
        }
        const auto [id, pointer_type] = buffer.Alias(PointerType::U32);
        const Id values = ctx.OpBitcast(ctx.U32[2], value);
        for (u32 c = 0; c < 2; ++c) {
            const Id index = ctx.OpIAdd(ctx.U32[1], address, ctx.ConstU32(c));
            const Id component = ctx.OpCompositeExtract(ctx.U32[1], values, c);
            ctx.BufferWrite(handle, PointerType::U32, original, index, [&] {
                ctx.OpStore(ctx.OpAccessChain(pointer_type, id, ctx.u32_zero_value, index), component);
            }, c);
        }
        return;
    }
    const Id original_index = address;
    const auto& spv_buffer = ctx.buffers[handle];
    if (const Id offset = spv_buffer.Offset(PointerSize::B64); Sirit::ValidId(offset)) {
        address = ctx.OpIAdd(ctx.U32[1], address, offset);
    }
    const auto [id, pointer_type] = spv_buffer.Alias(PointerType::U64);
    ctx.BufferWrite(handle, PointerType::U64, original_index, address, [&] {
        const Id ptr{ctx.OpAccessChain(pointer_type, id, ctx.u64_zero_value, address)};
        ctx.OpStore(ptr, value);
    });
}

void EmitStoreBufferF32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address, Id value) {
    EmitStoreBufferB32xN<1, PointerType::F32>(ctx, inst, handle, address, value);
}

void EmitStoreBufferF32x2(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address, Id value) {
    EmitStoreBufferB32xN<2, PointerType::F32>(ctx, inst, handle, address, value);
}

void EmitStoreBufferF32x3(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address, Id value) {
    EmitStoreBufferB32xN<3, PointerType::F32>(ctx, inst, handle, address, value);
}

void EmitStoreBufferF32x4(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address, Id value) {
    EmitStoreBufferB32xN<4, PointerType::F32>(ctx, inst, handle, address, value);
}

void EmitStoreBufferFormatF32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address, Id value) {
    UNREACHABLE_MSG("SPIR-V instruction");
}

void EmitGetScalarRegister(EmitContext&) {
    UNREACHABLE_MSG("Unreachable instruction");
}

void EmitSetScalarRegister(EmitContext&) {
    UNREACHABLE_MSG("Unreachable instruction");
}

void EmitGetVectorRegister(EmitContext& ctx) {
    UNREACHABLE_MSG("Unreachable instruction");
}

void EmitSetVectorRegister(EmitContext& ctx) {
    UNREACHABLE_MSG("Unreachable instruction");
}

void EmitSetVirtualRegister(EmitContext& ctx) {
    UNREACHABLE_MSG("Unreachable instruction");
}

void EmitGetVirtualRegister(EmitContext& ctx) {
    UNREACHABLE_MSG("Unreachable instruction");
}

void EmitSetGotoVariable(EmitContext&) {
    UNREACHABLE_MSG("Unreachable instruction");
}

void EmitGetGotoVariable(EmitContext&) {
    UNREACHABLE_MSG("Unreachable instruction");
}

} // namespace Shader::Backend::SPIRV
