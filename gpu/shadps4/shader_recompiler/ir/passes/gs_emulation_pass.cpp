// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: geometry-stage emulation roles (shader_recompiler/gs_emulation.h): what the compute
// roles cannot run, and the buffers of all roles. Runs after the last dead-code elimination,
// so only live operations are rejected.

#include "common/assert.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/ir/opcodes.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/runtime_info.h"

namespace Shader::Optimization {

namespace {
bool IsSharedMemoryOp(IR::Opcode op) {
    return op >= IR::Opcode::LoadSharedU16 && op <= IR::Opcode::SharedAtomicCmpSwap64;
}
} // namespace

void GsEmulationPass(IR::Program& program, const RuntimeInfo& runtime_info) {
    if (runtime_info.gs_role == GsEmulationRole::None) {
        return;
    }
    Info& info = program.info;
    if (runtime_info.gs_role == GsEmulationRole::EsCompute ||
        runtime_info.gs_role == GsEmulationRole::GsCompute) {
        // The compute roles run one invocation per vertex or primitive; invocations past the end
        // of the draw return first, so a workgroup barrier would not be reached by all of them.
        // On the PS4 the off-chip ES and GS have no LDS to share either. v_writelane is not lowered
        // for these stages (the graphics wave64 path rejects only reads of lanes >= 32).
        // (CopyStreamOut runs as one compute wave, lowered by LowerWave64Compute.)
        const char* stage = runtime_info.gs_role == GsEmulationRole::EsCompute ? "ES" : "GS";
        for (IR::Block* block : program.blocks) {
            for (const IR::Inst& inst : block->Instructions()) {
                ASSERT_MSG(inst.GetOpcode() != IR::Opcode::Barrier,
                           "Geometry emulation: {} shader {:#x} uses s_barrier, which the compute "
                           "emulation cannot run",
                           stage, info.pgm_hash);
                ASSERT_MSG(inst.GetOpcode() != IR::Opcode::WriteLane,
                           "Geometry emulation: {} shader {:#x} uses v_writelane, which the "
                           "compute emulation does not lower",
                           stage, info.pgm_hash);
                ASSERT_MSG(!IsSharedMemoryOp(inst.GetOpcode()),
                           "Geometry emulation: {} shader {:#x} uses LDS ({}), which the compute "
                           "emulation cannot run",
                           stage, info.pgm_hash, IR::NameOf(inst.GetOpcode()));
            }
        }
    }
    // ESGS/GSVS records, index runs and the draw header (written by the compute roles).
    info.buffers.push_back(BufferResource{
        .used_types = IR::Type::U32,
        .buffer_type = BufferType::GsScratch,
        .is_written = runtime_info.gs_role == GsEmulationRole::EsCompute ||
                      runtime_info.gs_role == GsEmulationRole::GsCompute,
    });
    if (runtime_info.gs_role == GsEmulationRole::EsCompute) {
        // Indexed draws: the guest index buffer; otherwise any valid range (not read).
        info.buffers.push_back(BufferResource{
            .used_types = IR::Type::U32,
            .buffer_type = BufferType::GsIndexInput,
        });
    }
}

} // namespace Shader::Optimization
