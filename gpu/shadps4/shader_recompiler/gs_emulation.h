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

#include <span>

#include "common/assert.h"
#include "common/types.h"
#include "shader_recompiler/ir/attribute.h"
#include "shader_recompiler/ir/opcodes.h"
#include "shader_recompiler/ir/microinstruction.h"
#include "shader_recompiler/ir/value.h"
#include "video_core/amdgpu/resource.h"
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

/// Point output: every emitted vertex is a primitive and a cut does nothing. The runs then hold
/// the emitted vertices padded with RestartIndex, and a compaction pass
/// (host_shaders/gs_point_compact.comp) turns them into one dense index list in PS4 order, drawn
/// as a point list (list topologies cannot restart on Apple).
inline bool PointOutput(AmdGpu::GsOutputPrimitiveType type) {
    return type == AmdGpu::GsOutputPrimitiveType::PointList;
}

/// Minimum vertices of a strip that draws something.
inline u32 MinStripVertices(AmdGpu::GsOutputPrimitiveType type) {
    switch (type) {
    case AmdGpu::GsOutputPrimitiveType::TriangleStrip:
        return 3;
    case AmdGpu::GsOutputPrimitiveType::LineStrip:
        return 2;
    case AmdGpu::GsOutputPrimitiveType::PointList:
        return 1;
    default:
        UNREACHABLE_MSG("Geometry emulation: GS output type {}", u32(type));
    }
}

/// Index entries per output slot: N vertices, plus one restart after each strip that draws
/// something (a cut after a shorter strip rewinds it instead), so at most N / M restarts. Points
/// need no restarts.
inline u32 IndexRunLength(u32 max_vert_out, AmdGpu::GsOutputPrimitiveType type) {
    if (PointOutput(type)) {
        return max_vert_out;
    }
    return max_vert_out + max_vert_out / MinStripVertices(type);
}

/// Gnm's copy-shader stream-out ABI, from the copy shader's input usage slots: the stream-out
/// buffer table (PtrSoBufferTable, usage 0x18) starts at s2, and no other input starts at s2 or
/// s3. The recompiler's stream-out store proof (V#s loaded through s[2:3]) rests on it.
inline bool SoTableAtS2(std::span<const u32> usage_slots) {
    bool table = false;
    for (const u32 slot : usage_slots) {
        const u32 usage = slot & 0xFF, start = (slot >> 16) & 0xFF;
        if (usage == 0x18 && start == 2) {
            table = true;
        } else if (start == 2 || start == 3) {
            return false;
        }
    }
    return table;
}

/// The stream-out buffer b whose base offset a CopyStreamOut store's soffset is, in the one shape
/// Gnm's copy shaders use: GsStreamOutOffset_b << 2 (the VGT's dword offset in bytes). -1 for
/// any other value.
inline int StreamOutOffsetBuffer(const IR::Value& soffset) {
    const IR::Inst* shift = soffset.TryInst();
    if (!shift || shift->GetOpcode() != IR::Opcode::ShiftLeftLogical32 ||
        !shift->Arg(1).IsImmediate() || shift->Arg(1).U32() != 2) {
        return -1;
    }
    const IR::Inst* input = shift->Arg(0).TryInst();
    if (!input || input->GetOpcode() != IR::Opcode::GetAttributeU32) {
        return -1;
    }
    const u64 attr = u64(input->Arg(0).Attribute());
    const u64 first = u64(IR::Attribute::GsStreamOutOffset0);
    return attr >= first && attr < first + 4 ? int(attr - first) : -1;
}

/// Why stream-out buffer `sharp` cannot bound the CopyStreamOut kernel's writes to a VGT buffer of
/// `size_dw` dwords and `stride_dw`-dword vertices, or null. The kernel writes draw-relative
/// vertex i (the wave's first vertex plus the lane; i < the vertices the compaction kernel found
/// to fit) at soffset = the buffer's offset at the draw's start * 4, plus i * V# stride, each
/// access within one V# stride (checked at compile time). With the V# stride equal to the VGT
/// stride, every write ends within VGT_STRMOUT_BUFFER_SIZE, so a V# range (the binding) of at
/// least that size holds all of them.
inline const char* StreamOutBindingGap(const AmdGpu::Buffer& sharp, u32 size_dw, u32 stride_dw) {
    const u64 size_bytes = u64(size_dw) * 4;
    if (sharp.swizzle_enable) {
        return "a swizzled stream-out V#";
    }
    if (u64(sharp.stride) != u64(stride_dw) * 4) {
        return "a stream-out V# stride other than VGT_STRMOUT_VTX_STRIDE";
    }
    if (size_bytes > UINT32_MAX || u64(sharp.stride) * sharp.num_records > UINT32_MAX) {
        return "a stream-out buffer beyond the 32-bit binding range";
    }
    if (u64(sharp.stride) * sharp.num_records < size_bytes) {
        return "a stream-out V# range shorter than VGT_STRMOUT_BUFFER_SIZE";
    }
    return nullptr;
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
    CompactBase,        ///< point output: dword offset of the compacted index list
    HeaderDwords = 16,
};

/// Indirect ES/GS draws: after the header, the setup kernel (gs_indirect_setup.comp) writes the
/// commands the counts it read from the guest's indirect arguments call for.
enum IndirectCommands : u32 {
    EsDispatch = 16,  ///< VkDispatchIndirectCommand of the ES
    GsDispatch = 19,  ///< VkDispatchIndirectCommand of the GS
    StripDraw = 22,   ///< VkDrawIndexedIndirectCommand of the copy-shader draw (firstIndex = runs)
    CompactDraw = 27, ///< point output: VkDrawIndexedIndirectCommand of the compacted point list
    /// Stream-out (point output only): the compaction kernel's stream-out tail writes these from
    /// the GPU-resident stream-out state, then advances that state.
    SoDispatch = 32, ///< VkDispatchIndirectCommand of the CopyStreamOut kernel (64 per group)
    SoCount = 35,    ///< vertices written (the compacted vertices that fit in every buffer)
    SoOffset0 = 36,  ///< SoOffset0 + n: buffer n's offset (dwords) at the start of the draw
    IndirectDwords = 48, ///< header + commands; the records follow (also for point output draws)
};

} // namespace Shader::GsEmulation
