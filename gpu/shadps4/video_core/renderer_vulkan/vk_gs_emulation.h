// SPDX-License-Identifier: GPL-2.0-or-later
// bbport (Apple): renderer side of the geometry-stage emulation (shader_recompiler/gs_emulation.h).
// An ES/GS draw records the ES and GS as compute dispatches over a per-draw scratch range, then
// draws the copy shader (the pipeline's vertex stage) over the GS's restart-separated strip runs.
#pragma once

#include <memory>
#include "shader_recompiler/gs_emulation.h"
#include <optional>
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace AmdGpu {
union Regs;
}

namespace Vulkan {

/// The compute half of an emulated ES/GS pipeline, owned by its GraphicsPipeline. The compute
/// pipelines are shared through the PipelineCache (keyed by stage permutation), which outlives it.
struct GsEmulationStages {
    const ComputePipeline* es;
    const ComputePipeline* gs;
    u32 verts_per_prim; ///< input vertices per primitive (list topologies)
    u32 invocations;    ///< GS instances
    u32 max_vert_out;   ///< VGT_GS_MAX_VERT_OUT
    u32 esgs_dwords;    ///< VGT_ESGS_RING_ITEMSIZE
    u32 vertex_dwords;  ///< VGT_GS_VERT_ITEMSIZE (stream 0)
    u32 run_length;     ///< index entries per output slot
};

/// What the emulation does not reproduce for a pipeline (registers of an ES/GS draw), or null.
const char* GsEmulationUnsupported(const AmdGpu::Regs& regs);

/// Input vertices per primitive of a supported GS input topology.
u32 GsEmulationVerticesPerPrimitive(const AmdGpu::Regs& regs);

/// Ranges bound to the GsScratch and GsIndexInput buffers of the draw being recorded.
struct GsEmulationBinding {
    vk::Buffer scratch{};
    u64 scratch_offset{};
    u64 scratch_size{};
    vk::Buffer index{};
    u64 index_offset{};
    u64 index_size{};
};

/// The emulated draw: an indexed strip draw over the GS's index runs.
struct GsEmulationDraw {
    vk::Buffer index_buffer{};
    u64 index_offset{};
    u32 index_count{};
    /// The render pass the compute passes ended, if one was open: a split only if the draw then
    /// begins the same pass again.
    std::optional<RenderState> ended_pass{};
};

} // namespace Vulkan
