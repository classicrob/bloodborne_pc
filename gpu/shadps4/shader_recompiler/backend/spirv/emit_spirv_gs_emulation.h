// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: SPIR-V side of the geometry-stage emulation (shader_recompiler/gs_emulation.h).
#pragma once

#include <optional>
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"

namespace Shader::Backend::SPIRV {

/// EsCompute, GsCompute and CopyStreamOut are emitted as GLCompute shaders.
bool IsGsComputeRole(const RuntimeInfo& runtime_info);

/// Stage interface of the compute roles (replaces the vertex/geometry inputs and outputs).
void DefineGsEmulationInterface(EmitContext& ctx);
/// Execution mode of the compute roles.
void DefineGsEmulationEntryPoint(EmitContext& ctx, Id main);
/// Start of main: grid bounds, record and slot addresses. No-op without a role.
void EmitGsEmulationPrologue(EmitContext& ctx);
/// End of main (GsCompute: close the strip, pad the index run). No-op otherwise.
void EmitGsEmulationEpilogue(EmitContext& ctx);

/// Ring record reads (GsCompute: ESGS, CopyVertex: GSVS); nullopt when not a role's ring read.
std::optional<Id> GsEmulationGetAttribute(EmitContext& ctx, IR::Attribute attr, u32 comp,
                                          u32 index);
/// System values of the compute roles; nullopt without a compute role.
std::optional<Id> GsEmulationGetAttributeU32(EmitContext& ctx, IR::Attribute attr);
/// CopyStreamOut's lane-valid bit; nullopt for other attributes.
std::optional<Id> GsEmulationGetAttributeU1(EmitContext& ctx, IR::Attribute attr);
/// A guest memory access predicate of this role: CopyStreamOut lanes the VGT would not launch
/// access nothing.
Id GsEmulationGuardAccess(EmitContext& ctx, Id predicate);
/// Ring record writes of the compute roles (CopyStreamOut: exports dropped); false without one.
bool GsEmulationSetAttribute(EmitContext& ctx, IR::Attribute attr, Id value, u32 comp);
/// GsCompute s_sendmsg gs emit / cut.
void GsEmulationEmitVertex(EmitContext& ctx);
void GsEmulationEmitPrimitive(EmitContext& ctx);

} // namespace Shader::Backend::SPIRV
