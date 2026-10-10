// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: SPIR-V side of the geometry-stage emulation (shader_recompiler/gs_emulation.h).
//
// EsCompute and GsCompute keep their GCN stage for translation (the ES sees VertexId/InstanceId,
// the GS its vertex offsets and primitive id) but are emitted as GLCompute shaders over the
// draw's scratch binding. CopyVertex is an ordinary vertex shader whose GSVS reads come from the
// scratch binding. Record dword d of a ring record is the IR attribute
// GsEmulation::RecordAttribute(d) (Position0, Param0, ... component d % 4).

#include "shader_recompiler/backend/spirv/emit_spirv_gs_emulation.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"
#include "shader_recompiler/gs_emulation.h"

namespace Shader::Backend::SPIRV {

namespace GsEmu = Shader::GsEmulation;

bool IsGsComputeRole(const RuntimeInfo& runtime_info) {
    return runtime_info.gs_role == GsEmulationRole::EsCompute ||
           runtime_info.gs_role == GsEmulationRole::GsCompute;
}

namespace {

u32 FindBuffer(const EmitContext& ctx, BufferType type) {
    for (u32 i = 0; i < ctx.buffers.size(); ++i) {
        if (ctx.buffers[i].buffer_type == type) {
            return i;
        }
    }
    UNREACHABLE_MSG("Geometry emulation: shader {:#x} has no buffer of type {}", ctx.info.pgm_hash,
                    u32(type));
}

Id ScratchPointer(EmitContext& ctx, Id dword_index) {
    const auto& [id, pointer_type] = ctx.buffers[ctx.gs.scratch].Alias(EmitContext::PointerType::U32);
    return ctx.OpAccessChain(pointer_type, id, ctx.u32_zero_value, dword_index);
}

Id LoadScratch(EmitContext& ctx, Id dword_index) {
    return ctx.OpLoad(ctx.U32[1], ScratchPointer(ctx, dword_index));
}

Id LoadHeader(EmitContext& ctx, GsEmu::Header field) {
    return LoadScratch(ctx, ctx.ConstU32(static_cast<u32>(field)));
}

void StoreScratch(EmitContext& ctx, Id dword_index, Id value) {
    ctx.OpStore(ScratchPointer(ctx, dword_index), value);
}

Id IAdd(EmitContext& ctx, Id a, Id b) {
    return ctx.OpIAdd(ctx.U32[1], a, b);
}

Id IMul(EmitContext& ctx, Id a, Id b) {
    return ctx.OpIMul(ctx.U32[1], a, b);
}

Id PrivateU32(EmitContext& ctx, const char* name) {
    const Id id = ctx.DefineVariable(ctx.U32[1], std::nullopt, spv::StorageClass::Private,
                                     ctx.u32_zero_value);
    ctx.Name(id, name);
    return id;
}

/// if (condition) { body(); }
template <typename Func>
void If(EmitContext& ctx, Id condition, Func&& body) {
    const Id then_label = ctx.OpLabel();
    const Id merge_label = ctx.OpLabel();
    ctx.OpSelectionMerge(merge_label, spv::SelectionControlMask::MaskNone);
    ctx.OpBranchConditional(condition, then_label, merge_label);
    ctx.AddLabel(then_label);
    body();
    ctx.OpBranch(merge_label);
    ctx.AddLabel(merge_label);
}

/// Invocations past the end of the grid return before any guest code runs.
void ReturnUnless(EmitContext& ctx, Id in_range) {
    const Id return_label = ctx.OpLabel();
    const Id merge_label = ctx.OpLabel();
    ctx.OpSelectionMerge(merge_label, spv::SelectionControlMask::MaskNone);
    ctx.OpBranchConditional(in_range, merge_label, return_label);
    ctx.AddLabel(return_label);
    ctx.OpReturn();
    ctx.AddLabel(merge_label);
}

/// Pulled index i of the draw (u16 indices are unpacked from dwords, little endian).
Id PullIndex(EmitContext& ctx, Id position) {
    const auto& [id, pointer_type] =
        ctx.buffers[ctx.gs.index_input].Alias(EmitContext::PointerType::U32);
    switch (ctx.runtime_info.gs_index_type) {
    case GsIndexType::U32:
        return ctx.OpLoad(ctx.U32[1],
                          ctx.OpAccessChain(pointer_type, id, ctx.u32_zero_value, position));
    case GsIndexType::U16: {
        const Id word = ctx.OpLoad(
            ctx.U32[1], ctx.OpAccessChain(pointer_type, id, ctx.u32_zero_value,
                                          ctx.OpShiftRightLogical(ctx.U32[1], position,
                                                                  ctx.ConstU32(1u))));
        const Id shift = ctx.OpShiftLeftLogical(
            ctx.U32[1], ctx.OpBitwiseAnd(ctx.U32[1], position, ctx.ConstU32(1u)),
            ctx.ConstU32(4u));
        return ctx.OpBitFieldUExtract(ctx.U32[1], word, shift, ctx.ConstU32(16u));
    }
    default:
        UNREACHABLE();
    }
}

u32 InputVerticesPerPrimitive(AmdGpu::PrimitiveType type) {
    switch (type) {
    case AmdGpu::PrimitiveType::PointList:
        return 1;
    case AmdGpu::PrimitiveType::LineList:
        return 2;
    case AmdGpu::PrimitiveType::TriangleList:
        return 3;
    default:
        UNREACHABLE_MSG("Geometry emulation: GS input primitive type {} is not implemented",
                        u32(type));
    }
}

void EsPrologue(EmitContext& ctx) {
    auto& gs = ctx.gs;
    const Id gid = ctx.OpLoad(ctx.U32[3], gs.global_invocation_id);
    const Id position = ctx.OpCompositeExtract(ctx.U32[1], gid, 0u);
    const Id instance = ctx.OpCompositeExtract(ctx.U32[1], gid, 1u);
    const Id records = LoadHeader(ctx, GsEmu::RecordsPerInstance);
    ReturnUnless(ctx, ctx.OpULessThan(ctx.U1[1], position, records));

    gs.base_vertex = LoadHeader(ctx, GsEmu::BaseVertex);
    gs.base_instance = LoadHeader(ctx, GsEmu::BaseInstance);
    const Id index = ctx.runtime_info.gs_index_type == GsIndexType::None
                         ? position
                         : PullIndex(ctx, IAdd(ctx, LoadHeader(ctx, GsEmu::FirstIndex), position));
    gs.vertex_id = IAdd(ctx, index, gs.base_vertex);
    gs.instance_id = IAdd(ctx, instance, gs.base_instance);
    const u32 stride = ctx.runtime_info.hw.es.vertex_data_size;
    ASSERT_MSG(stride != 0, "Geometry emulation: ESGS item size 0");
    gs.record_base =
        IAdd(ctx, LoadHeader(ctx, GsEmu::EsgsBase),
             IMul(ctx, IAdd(ctx, IMul(ctx, instance, records), position), ctx.ConstU32(stride)));
}

void GsPrologue(EmitContext& ctx) {
    auto& gs = ctx.gs;
    const auto& gs_info = ctx.runtime_info.hw.gs;
    const u32 invocations = std::max(gs_info.num_invocations, 1u);
    const u32 max_vert_out = ctx.runtime_info.gs_max_vert_out;
    const u32 run_length = GsEmu::IndexRunLength(max_vert_out, gs_info.out_primitive[0]);

    const Id gid = ctx.OpLoad(ctx.U32[3], gs.global_invocation_id);
    const Id primitive = ctx.OpCompositeExtract(ctx.U32[1], gid, 0u);
    const Id instance_and_invocation = ctx.OpCompositeExtract(ctx.U32[1], gid, 1u);
    const Id prims = LoadHeader(ctx, GsEmu::PrimsPerInstance);
    ReturnUnless(ctx, ctx.OpULessThan(ctx.U1[1], primitive, prims));

    const Id instance =
        ctx.OpUDiv(ctx.U32[1], instance_and_invocation, ctx.ConstU32(invocations));
    gs.primitive_id = primitive;
    gs.invocation_id = ctx.OpUMod(ctx.U32[1], instance_and_invocation, ctx.ConstU32(invocations));

    // Input records of this primitive (list topologies; ES records are per index position).
    const u32 per_prim = InputVerticesPerPrimitive(gs_info.in_primitive);
    const Id records = LoadHeader(ctx, GsEmu::RecordsPerInstance);
    gs.input_first_record =
        IAdd(ctx, IMul(ctx, instance, records), IMul(ctx, primitive, ctx.ConstU32(per_prim)));
    gs.esgs_base = LoadHeader(ctx, GsEmu::EsgsBase);

    // Output slot: (instance, primitive, GS invocation) in order.
    const Id slot = IAdd(ctx, IMul(ctx, IAdd(ctx, IMul(ctx, instance, prims), primitive),
                                   ctx.ConstU32(invocations)),
                         gs.invocation_id);
    gs.slot_first_vertex = IMul(ctx, slot, ctx.ConstU32(max_vert_out));
    gs.gsvs_base = LoadHeader(ctx, GsEmu::GsvsBase);
    gs.run_base = IAdd(ctx, LoadHeader(ctx, GsEmu::IndexBase),
                       IMul(ctx, slot, ctx.ConstU32(run_length)));
}

/// GSVS record of output vertex `vertex` of this invocation's slot.
Id GsvsDword(EmitContext& ctx, Id vertex_in_slot, u32 dword) {
    const u32 out_dwords = ctx.runtime_info.gs_vertex_dwords;
    return IAdd(ctx, ctx.gs.gsvs_base,
                IAdd(ctx, IMul(ctx, IAdd(ctx, ctx.gs.slot_first_vertex, vertex_in_slot),
                               ctx.ConstU32(out_dwords)),
                     ctx.ConstU32(dword)));
}

} // Anonymous namespace

void DefineGsEmulationInterface(EmitContext& ctx) {
    auto& gs = ctx.gs;
    gs.global_invocation_id = ctx.DefineVariable(ctx.U32[3], spv::BuiltIn::GlobalInvocationId,
                                                 spv::StorageClass::Input);
    if (ctx.runtime_info.gs_role == GsEmulationRole::GsCompute) {
        gs.emitted = PrivateU32(ctx, "gs_emitted");
        gs.run_pos = PrivateU32(ctx, "gs_run_pos");
        gs.strip_start = PrivateU32(ctx, "gs_strip_start");
        gs.strip_len = PrivateU32(ctx, "gs_strip_len");
    }
}

void DefineGsEmulationEntryPoint(EmitContext& ctx, Id main) {
    ctx.AddExecutionMode(main, spv::ExecutionMode::LocalSize, GsEmu::WorkgroupSize, 1u, 1u);
}

void EmitGsEmulationPrologue(EmitContext& ctx) {
    if (ctx.runtime_info.gs_role == GsEmulationRole::None) {
        return;
    }
    ctx.gs.scratch = FindBuffer(ctx, BufferType::GsScratch);
    switch (ctx.runtime_info.gs_role) {
    case GsEmulationRole::EsCompute:
        if (ctx.runtime_info.gs_index_type != GsIndexType::None) {
            ctx.gs.index_input = FindBuffer(ctx, BufferType::GsIndexInput);
        }
        EsPrologue(ctx);
        break;
    case GsEmulationRole::GsCompute:
        GsPrologue(ctx);
        break;
    case GsEmulationRole::CopyVertex:
        ctx.gs.gsvs_base = LoadHeader(ctx, GsEmu::GsvsBase);
        break;
    default:
        UNREACHABLE();
    }
}

std::optional<Id> GsEmulationGetAttribute(EmitContext& ctx, IR::Attribute attr, u32 comp,
                                          u32 index) {
    const auto role = ctx.runtime_info.gs_role;
    if (role == GsEmulationRole::None ||
        !(attr == IR::Attribute::Position0 || IR::IsParam(attr))) {
        return std::nullopt;
    }
    const u32 dword = GsEmu::RecordDword(attr, comp);
    Id address;
    switch (role) {
    case GsEmulationRole::GsCompute: {
        // ESGS record of input vertex `index` of this primitive.
        const u32 stride = ctx.runtime_info.hw.gs.in_vertex_data_size;
        ASSERT_MSG(index < InputVerticesPerPrimitive(ctx.runtime_info.hw.gs.in_primitive),
                   "Geometry emulation: GS reads input vertex {}", index);
        ASSERT_MSG(dword < stride, "Geometry emulation: GS reads ESGS dword {} of {}", dword,
                   stride);
        const Id record = IAdd(ctx, ctx.gs.input_first_record, ctx.ConstU32(index));
        address = IAdd(ctx, ctx.gs.esgs_base,
                       IAdd(ctx, IMul(ctx, record, ctx.ConstU32(stride)), ctx.ConstU32(dword)));
        break;
    }
    case GsEmulationRole::CopyVertex: {
        // GSVS record of the drawn vertex (the index runs name records directly).
        const Id vertex = ctx.OpLoad(ctx.U32[1], ctx.vertex_index);
        address = IAdd(ctx, ctx.gs.gsvs_base,
                       IAdd(ctx, IMul(ctx, vertex, ctx.ConstU32(ctx.runtime_info.gs_vertex_dwords)),
                            ctx.ConstU32(dword)));
        break;
    }
    default:
        UNREACHABLE_MSG("Geometry emulation: {} reads ring attribute {}", u32(role),
                        IR::NameOf(attr));
    }
    return ctx.OpBitcast(ctx.F32[1], LoadScratch(ctx, address));
}

std::optional<Id> GsEmulationGetAttributeU32(EmitContext& ctx, IR::Attribute attr) {
    switch (ctx.runtime_info.gs_role) {
    case GsEmulationRole::EsCompute:
        switch (attr) {
        case IR::Attribute::VertexId:
            return ctx.gs.vertex_id;
        case IR::Attribute::InstanceId:
            return ctx.gs.instance_id;
        case IR::Attribute::BaseVertex:
            return ctx.gs.base_vertex;
        case IR::Attribute::BaseInstance:
            return ctx.gs.base_instance;
        default:
            UNREACHABLE_MSG("Geometry emulation: ES reads attribute {}", IR::NameOf(attr));
        }
    case GsEmulationRole::GsCompute:
        switch (attr) {
        case IR::Attribute::PrimitiveId:
            return ctx.gs.primitive_id;
        case IR::Attribute::InvocationId:
            return ctx.gs.invocation_id;
        default:
            UNREACHABLE_MSG("Geometry emulation: GS reads attribute {}", IR::NameOf(attr));
        }
    default:
        return std::nullopt;
    }
}

bool GsEmulationSetAttribute(EmitContext& ctx, IR::Attribute attr, Id value, u32 comp) {
    const auto role = ctx.runtime_info.gs_role;
    if (!IsGsComputeRole(ctx.runtime_info)) {
        return false;
    }
    ASSERT_MSG(attr == IR::Attribute::Position0 || IR::IsParam(attr),
               "Geometry emulation: {} writes attribute {}", u32(role), IR::NameOf(attr));
    const u32 dword = GsEmu::RecordDword(attr, comp);
    const Id data = ctx.OpBitcast(ctx.U32[1], value);
    if (role == GsEmulationRole::EsCompute) {
        ASSERT_MSG(dword < ctx.runtime_info.hw.es.vertex_data_size,
                   "Geometry emulation: ES writes ESGS dword {} of {}", dword,
                   ctx.runtime_info.hw.es.vertex_data_size);
        StoreScratch(ctx, IAdd(ctx, ctx.gs.record_base, ctx.ConstU32(dword)), data);
        return true;
    }
    // GsCompute: the record of the next vertex to be emitted; emits past the bound are dropped.
    ASSERT_MSG(dword < ctx.runtime_info.gs_vertex_dwords,
               "Geometry emulation: GS writes GSVS dword {} of {}", dword,
               ctx.runtime_info.gs_vertex_dwords);
    const Id emitted = ctx.OpLoad(ctx.U32[1], ctx.gs.emitted);
    If(ctx,
       ctx.OpULessThan(ctx.U1[1], emitted, ctx.ConstU32(ctx.runtime_info.gs_max_vert_out)),
       [&] { StoreScratch(ctx, GsvsDword(ctx, emitted, dword), data); });
    return true;
}

void GsEmulationEmitVertex(EmitContext& ctx) {
    auto& gs = ctx.gs;
    const Id emitted = ctx.OpLoad(ctx.U32[1], gs.emitted);
    If(ctx,
       ctx.OpULessThan(ctx.U1[1], emitted, ctx.ConstU32(ctx.runtime_info.gs_max_vert_out)),
       [&] {
           const Id run_pos = ctx.OpLoad(ctx.U32[1], gs.run_pos);
           StoreScratch(ctx, IAdd(ctx, gs.run_base, run_pos),
                        IAdd(ctx, gs.slot_first_vertex, emitted));
           ctx.OpStore(gs.run_pos, IAdd(ctx, run_pos, ctx.ConstU32(1u)));
           ctx.OpStore(gs.strip_len,
                       IAdd(ctx, ctx.OpLoad(ctx.U32[1], gs.strip_len), ctx.ConstU32(1u)));
           ctx.OpStore(gs.emitted, IAdd(ctx, emitted, ctx.ConstU32(1u)));
       });
}

namespace {
/// Ends the current strip: a strip that draws something is closed with a restart index, a shorter
/// one is rewound (it draws nothing either way, and the run stays within its bound).
void EndStrip(EmitContext& ctx, bool closing) {
    auto& gs = ctx.gs;
    const u32 min_vertices = GsEmu::MinStripVertices(ctx.runtime_info.hw.gs.out_primitive[0]);
    const Id run_pos = ctx.OpLoad(ctx.U32[1], gs.run_pos);
    const Id draws = ctx.OpUGreaterThanEqual(ctx.U1[1], ctx.OpLoad(ctx.U32[1], gs.strip_len),
                                             ctx.ConstU32(min_vertices));
    if (closing) {
        If(ctx, draws, [&] {
            StoreScratch(ctx, IAdd(ctx, gs.run_base, run_pos), ctx.ConstU32(GsEmu::RestartIndex));
        });
    }
    const Id next = ctx.OpSelect(ctx.U32[1], draws,
                                 closing ? IAdd(ctx, run_pos, ctx.ConstU32(1u)) : run_pos,
                                 ctx.OpLoad(ctx.U32[1], gs.strip_start));
    ctx.OpStore(gs.run_pos, next);
    ctx.OpStore(gs.strip_start, next);
    ctx.OpStore(gs.strip_len, ctx.u32_zero_value);
}
} // Anonymous namespace

void GsEmulationEmitPrimitive(EmitContext& ctx) {
    EndStrip(ctx, true);
}

void EmitGsEmulationEpilogue(EmitContext& ctx) {
    if (ctx.runtime_info.gs_role != GsEmulationRole::GsCompute) {
        return;
    }
    // The end of the shader ends the strip; the rest of the run becomes restart indices.
    EndStrip(ctx, false);
    auto& gs = ctx.gs;
    const u32 run_length = GsEmu::IndexRunLength(ctx.runtime_info.gs_max_vert_out,
                                                 ctx.runtime_info.hw.gs.out_primitive[0]);
    const Id header = ctx.OpLabel();
    const Id check = ctx.OpLabel();
    const Id body = ctx.OpLabel();
    const Id continue_label = ctx.OpLabel();
    const Id merge = ctx.OpLabel();
    ctx.OpBranch(header);
    ctx.AddLabel(header);
    ctx.OpLoopMerge(merge, continue_label, spv::LoopControlMask::MaskNone);
    ctx.OpBranch(check);
    ctx.AddLabel(check);
    const Id run_pos = ctx.OpLoad(ctx.U32[1], gs.run_pos);
    ctx.OpBranchConditional(ctx.OpULessThan(ctx.U1[1], run_pos, ctx.ConstU32(run_length)), body,
                            merge);
    ctx.AddLabel(body);
    StoreScratch(ctx, IAdd(ctx, gs.run_base, run_pos), ctx.ConstU32(GsEmu::RestartIndex));
    ctx.OpBranch(continue_label);
    ctx.AddLabel(continue_label);
    ctx.OpStore(gs.run_pos, IAdd(ctx, run_pos, ctx.ConstU32(1u)));
    ctx.OpBranch(header);
    ctx.AddLabel(merge);
}

} // namespace Shader::Backend::SPIRV
