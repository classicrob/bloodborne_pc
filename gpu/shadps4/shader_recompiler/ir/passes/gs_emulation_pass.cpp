// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: buffers of the geometry-stage emulation roles (shader_recompiler/gs_emulation.h).

#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/runtime_info.h"

namespace Shader::Optimization {

void GsEmulationBuffers(IR::Program& program, const RuntimeInfo& runtime_info) {
    if (runtime_info.gs_role == GsEmulationRole::None) {
        return;
    }
    Info& info = program.info;
    // ESGS/GSVS records, index runs and the draw header (written by the compute roles).
    info.buffers.push_back(BufferResource{
        .used_types = IR::Type::U32,
        .buffer_type = BufferType::GsScratch,
        .is_written = runtime_info.gs_role != GsEmulationRole::CopyVertex,
    });
    if (runtime_info.gs_role == GsEmulationRole::EsCompute &&
        runtime_info.gs_index_type != GsIndexType::None) {
        info.buffers.push_back(BufferResource{
            .used_types = IR::Type::U32,
            .buffer_type = BufferType::GsIndexInput,
        });
    }
}

} // namespace Shader::Optimization
