// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: geometry-stage emulation on Apple (no geometry shaders in Metal/MoltenVK).
//
// An ES/GS draw runs as on the PS4, with both rings in memory:
//   1. the ES as a compute shader, one invocation per (index position, instance), writing its
//      ESGS ring record;
//   2. the GS as a compute shader, one invocation per (input primitive, instance, GS instance),
//      reading the ESGS records of its primitive and writing GSVS records into a fixed slot of
//      VGT_GS_MAX_VERT_OUT vertices, plus a run of strip indices (restart-separated);
//   3. the copy shader as the vertex shader of an indexed strip draw over those runs.
// Fixed slots keep the PS4 output order (input primitive, instance, GS instance, emission) without
// compaction. See GsEmulationRole (runtime_info.h) and docs/macos/geometry-shaders.md.
#pragma once

#include "common/assert.h"
#include "common/types.h"
#include "shader_recompiler/ir/attribute.h"
#include "video_core/amdgpu/regs_vertex.h"

namespace Shader::GsEmulation {

/// Invocations per workgroup of the ES and GS compute shaders.
constexpr u32 WorkgroupSize = 64;

/// Strip restart index (Metal always restarts strips on it).
constexpr u32 RestartIndex = 0xFFFFFFFFu;

/// A ring record is an array of dwords. In the IR of the three roles, record dword d is the
/// attribute RecordAttribute(d), component d % 4: Position0 for d < 4, then Param0, Param1, ...
/// This is the ESGS mapping RingAccessElimination already uses (dword = byte offset / 4); the GSVS
/// side uses the same encoding with d = the GCN GSVS component index.
constexpr u32 MaxRecordDwords = 4 + 4 * static_cast<u32>(IR::NumParams);

inline IR::Attribute RecordAttribute(u32 dword) {
    ASSERT_MSG(dword < MaxRecordDwords, "Geometry emulation: ring record dword {} out of range",
               dword);
    return dword < 4 ? IR::Attribute::Position0 : IR::Attribute::Param0 + (dword / 4 - 1);
}

inline u32 RecordDword(IR::Attribute attr, u32 comp) {
    if (attr == IR::Attribute::Position0) {
        return comp;
    }
    ASSERT_MSG(IR::IsParam(attr), "Geometry emulation: attribute {} is not a ring record dword",
               IR::NameOf(attr));
    return 4 + (static_cast<u32>(attr) - static_cast<u32>(IR::Attribute::Param0)) * 4 + comp;
}

/// Minimum vertices of a strip that draws something.
inline u32 MinStripVertices(AmdGpu::GsOutputPrimitiveType type) {
    switch (type) {
    case AmdGpu::GsOutputPrimitiveType::TriangleStrip:
        return 3;
    case AmdGpu::GsOutputPrimitiveType::LineStrip:
        return 2;
    default:
        UNREACHABLE_MSG("Geometry emulation: GS output type {} is not a strip", u32(type));
    }
}

/// Index entries per output slot: N vertices, plus one restart after each strip that draws
/// something (a cut after a shorter strip rewinds it instead), so at most N / M restarts.
inline u32 IndexRunLength(u32 max_vert_out, AmdGpu::GsOutputPrimitiveType type) {
    return max_vert_out + max_vert_out / MinStripVertices(type);
}

/// Header::IndexKind values.
enum class IndexKind : u32 {
    None = 0, ///< non-indexed: VertexId = position + BaseVertex
    U16 = 1,
    U32 = 2,
};

/// Dword layout of the header at the start of a draw's GsScratch binding.
enum Header : u32 {
    RecordsPerInstance, ///< V: index positions (ES invocations) per instance
    PrimsPerInstance,   ///< P: input primitives per instance
    Instances,          ///< I: draw instances
    IndexKind,          ///< IndexKind: how VertexId is pulled
    BaseVertex,         ///< added to the pulled index (vertexOffset / firstVertex)
    BaseInstance,       ///< firstInstance
    FirstIndex,         ///< first index element in the GsIndexInput binding
    EsgsBase,           ///< dword offset of the ESGS records
    GsvsBase,           ///< dword offset of the GSVS records
    IndexBase,          ///< dword offset of the index runs
    HeaderDwords = 16,
};

} // namespace Shader::GsEmulation
