// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/assert.h"
#include "shader_recompiler/gs_emulation.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/opcodes.h"
#include "shader_recompiler/ir/operand_helper.h"
#include "shader_recompiler/ir/position.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/ir/reg.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/runtime_info.h"

namespace Shader::Optimization {

/// bbport: the copy shader of an emulated GS draw (GsEmulationRole::CopyVertex). Its GSVS ring
/// loads become reads of GSVS record dword d (GsEmulation::RecordAttribute), d being the GCN GSVS
/// component index: the load's byte offset (instruction offset + soffset) is d * N * 64, N =
/// VGT_GS_MAX_VERT_OUT (64 lanes of N dwords per component). The copy shader's own exports then
/// map position, misc and parameter outputs exactly as the hardware VS does.
static void CopyVertexRingLoads(const IR::Program& program, const RuntimeInfo& runtime_info) {
    const u32 component_bytes = runtime_info.gs_max_vert_out * 64u;
    ASSERT(component_bytes != 0);
    for (IR::Block* block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() != IR::Opcode::LoadBufferU32) {
                continue;
            }
            const auto flags = inst.Flags<IR::BufferInstInfo>();
            if (!flags.system_coherent || !flags.globally_coherent) {
                continue;
            }
            const IR::Value soffset = IR::GetBufferSOffsetArg(&inst);
            ASSERT_MSG(soffset.IsImmediate(),
                       "Geometry emulation: copy shader GSVS load with a dynamic soffset");
            const u32 offset = flags.inst_offset.Value() + soffset.U32();
            ASSERT_MSG(offset % component_bytes == 0,
                       "Geometry emulation: copy shader GSVS load at {:#x} is not a component start "
                       "(component size {:#x})",
                       offset, component_bytes);
            const u32 dword = offset / component_bytes;
            ASSERT_MSG(dword < runtime_info.gs_vertex_dwords,
                       "Geometry emulation: copy shader reads GSVS component {} of {}", dword,
                       runtime_info.gs_vertex_dwords);
            IR::IREmitter ir{*block, IR::Block::InstructionList::s_iterator_to(inst)};
            const auto value = ir.GetAttribute(GsEmulation::RecordAttribute(dword), dword % 4);
            inst.ReplaceOpcode(IR::Opcode::BitCastU32F32);
            inst.ClearArgs();
            inst.SetArg(0, value);
        }
    }
}

void RingAccessElimination(const IR::Program& program, const RuntimeInfo& runtime_info) {
    auto& info = program.info;
    if (runtime_info.gs_role == GsEmulationRole::CopyVertex) {
        CopyVertexRingLoads(program, runtime_info);
        return;
    }

    const auto& ForEachInstruction = [&](auto func) {
        for (IR::Block* block : program.blocks) {
            for (IR::Inst& inst : block->Instructions()) {
                IR::IREmitter ir{*block, IR::Block::InstructionList::s_iterator_to(inst)};
                func(ir, inst);
            }
        }
    };

    switch (program.info.hw_stage) {
    case HwStage::Local: {
        ForEachInstruction([=](IR::IREmitter& ir, IR::Inst& inst) {
            const auto opcode = inst.GetOpcode();
            switch (opcode) {
            case IR::Opcode::WriteSharedU64:
            case IR::Opcode::WriteSharedU32: {
                bool is_composite = opcode == IR::Opcode::WriteSharedU64;
                u32 num_components = opcode == IR::Opcode::WriteSharedU32 ? 1 : 2;

                ASSERT(inst.Arg(0).IsImmediate());

                u32 offset = inst.Arg(0).U32();
                IR::Value data =
                    is_composite ? ir.UnpackUint2x32(IR::U64{inst.Arg(1)}) : inst.Arg(1);
                for (s32 i = 0; i < num_components; i++) {
                    const auto attrib = IR::Attribute::Param0 + (offset / 16);
                    const auto comp = (offset / 4) % 4;
                    const IR::U32 value =
                        IR::U32{is_composite ? ir.CompositeExtract(data, i) : data};
                    ir.SetAttribute(attrib, ir.BitCast<IR::F32, IR::U32>(value), comp);
                    offset += 4;
                }
                inst.Invalidate();
                break;
            }
            default:
                break;
            }
        });
        break;
    }
    case HwStage::Export: {
        ForEachInstruction([=](IR::IREmitter& ir, IR::Inst& inst) {
            const auto opcode = inst.GetOpcode();
            switch (opcode) {
            case IR::Opcode::StoreBufferU32: {
                const auto info = inst.Flags<IR::BufferInstInfo>();
                if (!info.system_coherent || !info.globally_coherent) {
                    break;
                }

                const auto offset = inst.Flags<IR::BufferInstInfo>().inst_offset.Value();
                ASSERT(offset < runtime_info.hw.es.vertex_data_size * 4);
                const auto data = ir.BitCast<IR::F32>(IR::U32{inst.Arg(2)});
                const auto attrib =
                    IR::Value{offset < 16 ? IR::Attribute::Position0
                                          : IR::Attribute::Param0 + (offset / 16 - 1)};
                const u32 comp = (offset / 4) % 4;

                inst.ReplaceOpcode(IR::Opcode::SetAttribute);
                inst.ClearArgs();
                inst.SetArg(0, attrib);
                inst.SetArg(1, data);
                inst.SetArg(2, ir.Imm32(comp));
                break;
            }
            default:
                break;
            }
        });
        break;
    }
    case HwStage::Geometry: {
        const auto& gs_info = runtime_info.hw.gs;
        info.gs_copy_data = Shader::ParseCopyShader(gs_info.vs_copy);

        u32 dwords_per_vertex = gs_info.out_vertex_data_size;
        if (info.gs_copy_data.num_comps && info.gs_copy_data.num_comps > dwords_per_vertex) {
            LOG_WARNING(Render_Vulkan,
                        "VERT_ITEMSIZE {} is different than actual number of dwords per vertex {}",
                        dwords_per_vertex, info.gs_copy_data.num_comps);
            dwords_per_vertex = info.gs_copy_data.num_comps;
        }
        if (runtime_info.gs_role == GsEmulationRole::GsCompute) {
            ASSERT_MSG(dwords_per_vertex == runtime_info.gs_vertex_dwords,
                       "Geometry emulation: the copy shader reads {} GSVS dwords per vertex, "
                       "VGT_GS_VERT_ITEMSIZE is {}",
                       dwords_per_vertex, runtime_info.gs_vertex_dwords);
        }

        ForEachInstruction([&](IR::IREmitter& ir, IR::Inst& inst) {
            const auto opcode = inst.GetOpcode();
            switch (opcode) {
            case IR::Opcode::LoadBufferU32: {
                const auto info = inst.Flags<IR::BufferInstInfo>();
                if (!info.system_coherent || !info.globally_coherent) {
                    break;
                }

                const auto vertex_id = (info.index_enable ? IR::GetBufferIndexArg(&inst)
                                                          : IR::GetBufferVOffsetArg(&inst))
                                           .U32() >>
                                       2;
                const auto soffset = IR::GetBufferSOffsetArg(&inst);
                const auto bucket = soffset.U32() / 256u;
                const auto attrib = bucket < 4 ? IR::Attribute::Position0
                                               : IR::Attribute::Param0 + (bucket / 4 - 1);
                const auto comp = bucket % 4;

                auto attr_value = ir.GetAttribute(attrib, comp, vertex_id);
                inst.ReplaceOpcode(IR::Opcode::BitCastU32F32);
                inst.ClearArgs();
                inst.SetArg(0, attr_value);
                break;
            }
            case IR::Opcode::StoreBufferU32: {
                const auto buffer_info = inst.Flags<IR::BufferInstInfo>();
                if (!buffer_info.system_coherent || !buffer_info.globally_coherent) {
                    break;
                }

                const auto offset = inst.Flags<IR::BufferInstInfo>().inst_offset.Value();
                const auto data = ir.BitCast<IR::F32>(IR::U32{inst.Arg(2)});
                if (runtime_info.gs_role == GsEmulationRole::GsCompute) {
                    // GSVS record dword d = the GCN component index; the store belongs to the
                    // next emitted vertex (as on the native path). Position and misc outputs
                    // stay raw: the copy shader maps them.
                    const u32 dword = (offset / (gs_info.output_vertices * 4u)) % dwords_per_vertex;
                    inst.Invalidate();
                    ir.SetAttribute(GsEmulation::RecordAttribute(dword), data, dword % 4);
                    break;
                }
                const auto comp_ofs = info.gs_copy_data.output_vertices * sizeof(u32);
                const auto output_size = gs_info.output_vertices * dwords_per_vertex * sizeof(u32);

                const auto vc_read_ofs = (((offset / comp_ofs) * comp_ofs) % output_size) * 16u;
                const auto& it = info.gs_copy_data.attr_map.find(vc_read_ofs);
                ASSERT(it != info.gs_copy_data.attr_map.cend());
                const auto& [attr, comp] = it->second;

                inst.Invalidate();
                if (IsPosition(attr)) {
                    ExportPosition(ir, gs_info, false, attr, comp, data);
                } else {
                    ir.SetAttribute(attr, data, comp);
                }
                break;
            }
            default:
                break;
            }
        });
        break;
    }
    default:
        break;
    }
}

} // namespace Shader::Optimization
