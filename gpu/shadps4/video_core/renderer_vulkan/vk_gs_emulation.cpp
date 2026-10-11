// SPDX-License-Identifier: GPL-2.0-or-later
// bbport (Apple): renderer side of the geometry-stage emulation (vk_gs_emulation.h,
// shader_recompiler/gs_emulation.h).

#include <array>
#include <cstring>
#include "common/alignment.h"
#include "common/assert.h"
#include "common/div_ceil.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/renderer_vulkan/vk_gs_emulation.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

namespace GsEmu = Shader::GsEmulation;

const char* GsEmulationUnsupported(const AmdGpu::Regs& regs) {
    if (regs.vgt_gs_mode.mode != AmdGpu::GsScenario::ScenarioG) {
        return "a VGT_GS_MODE other than scenario G";
    }
    if (regs.vgt_gs_mode.onchip) {
        return "on-chip geometry shaders (ESGS ring in LDS)";
    }
    if (regs.vgt_strmout_config.raw) {
        return "stream-out";
    }
    if (regs.vgt_gs_instance_cnt.IsEnabled()) {
        return "geometry shader instancing";
    }
    if (regs.vgt_gs_out_prim_type.unique_type_per_stream) {
        return "per-stream output primitive types";
    }
    switch (regs.vgt_gs_out_prim_type.GetPrimitiveType(0)) {
    case AmdGpu::GsOutputPrimitiveType::TriangleStrip:
    case AmdGpu::GsOutputPrimitiveType::LineStrip:
        break;
    default:
        return "point-list geometry shader output";
    }
    switch (regs.primitive_type) {
    case AmdGpu::PrimitiveType::PointList:
    case AmdGpu::PrimitiveType::LineList:
    case AmdGpu::PrimitiveType::TriangleList:
        break;
    default:
        return "this geometry shader input topology (only point, line and triangle lists)";
    }
    if (regs.vgt_gs_max_vert_out == 0 || regs.vgt_esgs_ring_itemsize == 0 ||
        regs.vgt_gs_vert_itemsize[0] == 0) {
        return "a zero GS output bound or ring item size";
    }
    return nullptr;
}

u32 GsEmulationVerticesPerPrimitive(const AmdGpu::Regs& regs) {
    switch (regs.primitive_type) {
    case AmdGpu::PrimitiveType::PointList:
        return 1;
    case AmdGpu::PrimitiveType::LineList:
        return 2;
    case AmdGpu::PrimitiveType::TriangleList:
        return 3;
    default:
        UNREACHABLE_MSG("Geometry emulation: input topology {}", u32(regs.primitive_type));
    }
}

/// Largest scratch range of one draw (ESGS + GSVS records and index runs). The stream buffer it is
/// taken from is 128 MiB; a draw needing more stops with its sizes rather than being dropped.
constexpr u64 MaxScratchBytes = 32_MB;

GsEmulationDraw Rasterizer::RecordGsEmulation(const GraphicsPipeline* pipeline,
                                              const AmdGpu::Regs& regs, bool is_indexed,
                                              u32 index_offset) {
    const auto& stages = *pipeline->GsEmulation();

    const u32 records = regs.num_indices;
    const u32 instances = regs.num_instances.NumInstances();
    // A trailing partial primitive is not assembled (as on the hardware).
    const u32 prims = records / stages.verts_per_prim;
    const u64 slots = u64(prims) * instances * stages.invocations;
    const u64 esgs_base = GsEmu::HeaderDwords;
    const u64 gsvs_base = esgs_base + u64(records) * instances * stages.esgs_dwords;
    const u64 index_base = gsvs_base + slots * stages.max_vert_out * stages.vertex_dwords;
    const u64 bytes = (index_base + slots * stages.run_length) * 4;
    ASSERT_MSG(bytes <= MaxScratchBytes,
               "macOS geometry emulation: draw needs {} bytes of scratch ({} index positions x {} "
               "instances, {} output slots of {} vertices x {} dwords), limit {}",
               bytes, records, instances, slots, stages.max_vert_out, stages.vertex_dwords,
               MaxScratchBytes);

    std::array<u32, GsEmu::HeaderDwords> header{};
    header[GsEmu::RecordsPerInstance] = records;
    header[GsEmu::PrimsPerInstance] = prims;
    header[GsEmu::Instances] = instances;
    // Vertex pulling: the base vertex is VGT_INDX_OFFSET, the base instance 0 (GetDrawOffsets).
    header[GsEmu::BaseVertex] = regs.index_offset;
    header[GsEmu::BaseInstance] = 0;
    header[GsEmu::EsgsBase] = u32(esgs_base);
    header[GsEmu::GsvsBase] = u32(gsvs_base);
    header[GsEmu::IndexBase] = u32(index_base);

    const u64 alignment = instance.StorageMinAlignment();
    auto& stream = buffer_cache.GetStreamBuffer();
    const auto [data, scratch_offset] = stream.Map(bytes, alignment);
    ASSERT_MSG(data, "macOS geometry emulation: no host mapping for {} bytes of scratch", bytes);

    gs_binding = {.scratch = stream.Handle(),
                  .scratch_offset = scratch_offset,
                  .scratch_size = bytes,
                  .index = stream.Handle(),
                  .index_offset = scratch_offset,
                  .index_size = bytes};
    if (is_indexed) {
        ASSERT_MSG((regs.enable_primitive_restart & 1) == 0,
                   "macOS geometry emulation: indexed ES/GS draw with primitive restart");
        ASSERT_MSG(regs.index_buffer_type.swap_mode == AmdGpu::IndexSwapMode::None,
                   "macOS geometry emulation: index swap mode {}",
                   u32(regs.index_buffer_type.swap_mode));
        const bool index16 = regs.index_buffer_type.index_type == AmdGpu::IndexType::Index16;
        const u32 index_size = index16 ? 2 : 4;
        const VAddr address =
            regs.index_base_address.Address<VAddr>() + u64(index_offset) * index_size;
        const u32 index_bytes = records * index_size;
        const auto [buffer, offset] = buffer_cache.ObtainBuffer(address, index_bytes, false);
        needs_barrier |= runtime.IsBufferAccessed(buffer, offset, index_bytes);
        // Bound from the storage-aligned start; FirstIndex skips the elements before the draw's
        // first index. The shader reads whole dwords (u16 indices in pairs): the range is rounded
        // up to a dword, which must stay inside the cached buffer.
        const u64 bind_offset = Common::AlignDown(offset, alignment);
        const u64 skip = offset - bind_offset;
        ASSERT_MSG(skip % index_size == 0,
                   "macOS geometry emulation: index buffer at {:#x} is not {}-byte aligned",
                   address, index_size);
        const u64 bind_size = Common::AlignUp(skip + index_bytes, 4);
        ASSERT_MSG(bind_offset + bind_size <= buffer->SizeBytes(),
                   "macOS geometry emulation: index range {:#x}+{} rounded to dwords leaves its "
                   "cached buffer",
                   address, index_bytes);
        header[GsEmu::IndexKind] = u32(index16 ? GsEmu::IndexKind::U16 : GsEmu::IndexKind::U32);
        header[GsEmu::FirstIndex] = u32(skip / index_size);
        gs_binding.index = buffer->Handle();
        gs_binding.index_offset = bind_offset;
        gs_binding.index_size = bind_size;
    } else {
        header[GsEmu::IndexKind] = u32(GsEmu::IndexKind::None);
    }
    std::memcpy(data, header.data(), sizeof(header));
    stream.Commit();

    gs_stats.draws.fetch_add(1, std::memory_order_relaxed);
    gs_stats.scratch_bytes.fetch_add(bytes, std::memory_order_relaxed);
    if (prims == 0) {
        return {};
    }
    std::optional<RenderState> ended_pass;
    if (scheduler.IsRendering()) {
        ended_pass = scheduler.GetRenderState();
    }

    const auto dispatch = [&](const ComputePipeline& compute, u32 groups_x, u32 groups_y) {
        const bool bound = BindResources(&compute);
        ASSERT_MSG(bound, "macOS geometry emulation: binding the {} stage failed",
                   &compute == stages.es ? "ES" : "GS");
        if (needs_barrier) {
            runtime.FlushBarriers();
        }
        scheduler.EndRendering();
        compute.BindResources(set_writes, push_data);
        const vk::Pipeline handle = compute.Handle();
        scheduler.Record([=](vk::CommandBuffer cmdbuf) {
            cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, handle);
            cmdbuf.dispatch(groups_x, groups_y, 1);
        });
        ResetBindings(true);
        gs_stats.dispatches.fetch_add(1, std::memory_order_relaxed);
    };
    const auto barrier = [&](vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access) {
        scheduler.Record([=](vk::CommandBuffer cmdbuf) {
            const vk::MemoryBarrier2 memory = {
                .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
                .dstStageMask = dst_stage,
                .dstAccessMask = dst_access,
            };
            cmdbuf.pipelineBarrier2(vk::DependencyInfo{
                .memoryBarrierCount = 1,
                .pMemoryBarriers = &memory,
            });
        });
    };

    dispatch(*stages.es, Common::DivCeil(records, GsEmu::WorkgroupSize), instances);
    barrier(vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderStorageRead);
    dispatch(*stages.gs, Common::DivCeil(prims, GsEmu::WorkgroupSize),
             instances * stages.invocations);
    barrier(vk::PipelineStageFlagBits2::eVertexShader | vk::PipelineStageFlagBits2::eIndexInput,
            vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eIndexRead);

    return {.index_buffer = stream.Handle(),
            .index_offset = scratch_offset + index_base * 4,
            .index_count = u32(slots * stages.run_length),
            .ended_pass = ended_pass};
}

} // namespace Vulkan
