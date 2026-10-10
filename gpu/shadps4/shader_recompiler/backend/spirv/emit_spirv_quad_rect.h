// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <vector>
#include "common/types.h"

namespace Shader {
struct HwFragmentRuntimeInfo;
struct Info;
struct RuntimeInfo;
}

namespace Shader::Backend::SPIRV {

enum class AuxShaderType : u32 {
    RectListTCS,
    QuadListTCS,
    PassthroughTES,
};

/// Varying locations the fragment stage declares as inputs (bit N = Location N), including the
/// motion-vector locations. The auxiliary stages forward exactly these. Not valid with clip-distance
/// emulation, which shifts every parameter location by one.
[[nodiscard]] u32 AuxiliaryVaryingLocations(const HwFragmentRuntimeInfo& fs_info);

/// Varying locations a vertex or tessellation-evaluation stage writes (bit N = Location N), as
/// EmitContext::DefineOutputs declares them: a vec4 per stored parameter, plus the motion-vector
/// pair only from a hardware vertex shader with motion enabled (EmitContext::VertexMotion). Not
/// valid with clip-distance emulation.
[[nodiscard]] u32 PreRasterOutputLocations(const Info& info, const RuntimeInfo& runtime);

/// vs_output_locations: bit N set when the vertex stage writes a vec4 at Location N. GCN lets the
/// pixel shader read a parameter the vertex shader never exported (value undefined); Vulkan requires
/// every input location to be written by the previous stage, so such locations are not read from
/// the vertex stage and are forwarded as zero instead.
[[nodiscard]] std::vector<u32> EmitAuxilaryTessShader(AuxShaderType type,
                                                      const HwFragmentRuntimeInfo& fs_info,
                                                      u32 vs_output_locations);

} // namespace Shader::Backend::SPIRV
