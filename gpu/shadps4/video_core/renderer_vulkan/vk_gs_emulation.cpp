// SPDX-License-Identifier: GPL-2.0-or-later
// bbport (Apple): renderer side of the geometry-stage emulation (vk_gs_emulation.h,
// shader_recompiler/gs_emulation.h).

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "common/alignment.h"
#include "common/assert.h"
#include "common/div_ceil.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/host_shaders/gs_indirect_setup_comp.h"
#include "video_core/host_shaders/gs_point_compact_comp.h"
#include "video_core/renderer_vulkan/vk_gs_emulation.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan {

namespace GsEmu = Shader::GsEmulation;

namespace {
const bool gs_skip_draws = [] {
    const char* value = std::getenv("BB_GS_SKIP_DRAWS");
    const bool on = value && value[0] == '1';
    if (on) {
        std::fprintf(stderr, "\n*** BB_GS_SKIP_DRAWS=1: DIAGNOSTIC MODE. Every emulated ES/GS draw "
                             "is skipped; rendering is wrong by design. ***\n\n");
    }
    return on;
}();
std::atomic<u64> gs_skipped_total{0}, gs_skipped_window{0};
} // namespace

bool GsSkipDraws() {
    return gs_skip_draws;
}

void GsSkipDrawsCount() {
    gs_skipped_total.fetch_add(1, std::memory_order_relaxed);
    gs_skipped_window.fetch_add(1, std::memory_order_relaxed);
}

void GsSkipDrawsOnFlip() {
    static u64 flips = 0;
    if (gs_skip_draws && ++flips % 600 == 0) {
        std::printf("GS draws SKIPPED: %llu in %llu frames (BB_GS_SKIP_DRAWS diagnostic mode, "
                    "rendering is wrong by design)\n",
                    (unsigned long long)gs_skipped_total.load(std::memory_order_relaxed),
                    (unsigned long long)flips);
    }
}

void GsSkipDrawsOnWindow() {
    if (gs_skip_draws) {
        std::printf("GS draws SKIPPED: %llu in this window (BB_GS_SKIP_DRAWS diagnostic mode)\n",
                    (unsigned long long)gs_skipped_window.exchange(0, std::memory_order_relaxed));
    }
}

const char* GsEmulationUnsupported(const AmdGpu::Regs& regs) {
    if (regs.vgt_gs_mode.mode != AmdGpu::GsScenario::ScenarioG) {
        return "a VGT_GS_MODE other than scenario G";
    }
    if (regs.vgt_gs_mode.onchip) {
        return "on-chip geometry shaders (ESGS ring in LDS)";
    }
    if (regs.vgt_strmout_config.raw) {
        // The modelled subset (GS-DESIGN 11): stream 0 alone, rasterizing stream 0, of points
        // (the compacted list is the PS4 write order; strips would be written as lists), with the
        // copy shader's stream-out SGPRs naming exactly the enabled buffers. (Gnm sets SO_EN on
        // copy shaders whether or not a draw streams out; only a stream-out draw is checked.)
        const auto& buffers = regs.vgt_strmout_buffer_config;
        if (regs.vgt_strmout_config.raw != 1) {
            return "stream-out other than stream 0 alone, rasterizing stream 0";
        }
        if (!regs.vs_program.settings.so_en) {
            return "stream-out with a copy shader without stream-out SGPRs";
        }
        if (buffers.stream_0_buf_en == 0 || buffers.stream_1_buf_en || buffers.stream_2_buf_en ||
            buffers.stream_3_buf_en) {
            return "stream-out buffers other than some for stream 0";
        }
        if (regs.vs_program.settings.so_base_en != buffers.stream_0_buf_en) {
            return "stream-out with copy shader offsets for other buffers than the enabled ones";
        }
        if (!GsEmu::PointOutput(regs.vgt_gs_out_prim_type.GetPrimitiveType(0))) {
            return "stream-out of geometry shader strips";
        }
        const u32* copy = regs.vs_program.Address<const u32*>();
        if (!copy || !GsEmu::SoTableAtS2(AmdGpu::InputUsageSlots(copy))) {
            return "stream-out with a copy shader whose stream-out buffer table is not at s[2:3]";
        }
        for (u32 buffer = 0; buffer < 4; ++buffer) {
            if (((buffers.stream_0_buf_en >> buffer) & 1) &&
                regs.vgt_strmout_buffers[buffer].vtx_stride_dw == 0) {
                return "stream-out with a zero vertex stride";
            }
        }
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
    case AmdGpu::GsOutputPrimitiveType::PointList:
        break;
    default:
        return "this geometry shader output primitive type";
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
    // Point output draws through the compaction kernel's indirect command after the header.
    const u64 esgs_base = stages.point_output ? GsEmu::IndirectDwords : GsEmu::HeaderDwords;
    const u64 gsvs_base = esgs_base + u64(records) * instances * stages.esgs_dwords;
    const u64 index_base = gsvs_base + slots * stages.max_vert_out * stages.vertex_dwords;
    const u64 compact_base = index_base + slots * stages.run_length;
    const u64 bytes = (compact_base + (stages.point_output ? slots * stages.run_length : 0)) * 4;
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
    header[GsEmu::CompactBase] = u32(compact_base);

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
    if (stages.point_output) {
        // An empty point draw until the compaction kernel writes it.
        std::memset(static_cast<u8*>(data) + GsEmu::CompactDraw * 4, 0,
                    sizeof(VkDrawIndexedIndirectCommand));
    }
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

    RecordGsDispatch(*stages.es, Common::DivCeil(records, GsEmu::WorkgroupSize), instances);
    RecordGsBarrier(vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderStorageRead);
    RecordGsDispatch(*stages.gs, Common::DivCeil(prims, GsEmu::WorkgroupSize),
                     instances * stages.invocations);
    if (stages.point_output) {
        RecordGsBarrier(vk::PipelineStageFlagBits2::eComputeShader,
                        vk::AccessFlagBits2::eShaderStorageRead |
                            vk::AccessFlagBits2::eShaderStorageWrite);
        RecordGsPointCompaction(stages, regs);
        if (stages.so) {
            RecordGsStreamOut(stages, regs, stream.Handle(), scratch_offset);
            if (regs.clipper_control.dx_rasterization_kill) {
                return {.ended_pass = ended_pass, .skip_draw = true};
            }
        }
        RecordGsBarrier(vk::PipelineStageFlagBits2::eVertexShader |
                            vk::PipelineStageFlagBits2::eIndexInput |
                            vk::PipelineStageFlagBits2::eDrawIndirect,
                        vk::AccessFlagBits2::eShaderStorageRead |
                            vk::AccessFlagBits2::eIndexRead |
                            vk::AccessFlagBits2::eIndirectCommandRead);
        return {.index_buffer = stream.Handle(),
                .index_offset = scratch_offset,
                .indirect = true,
                .indirect_offset = scratch_offset + GsEmu::CompactDraw * 4,
                .ended_pass = ended_pass};
    }
    RecordGsBarrier(vk::PipelineStageFlagBits2::eVertexShader |
                        vk::PipelineStageFlagBits2::eIndexInput,
                    vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eIndexRead);

    return {.index_buffer = stream.Handle(),
            .index_offset = scratch_offset + index_base * 4,
            .index_count = u32(slots * stages.run_length),
            .ended_pass = ended_pass};
}

void Rasterizer::RecordGsDispatch(const ComputePipeline& compute, u32 groups_x, u32 groups_y,
                                  vk::Buffer indirect, u64 indirect_offset) {
    const auto& stages = compute.GetStage(Shader::SwStage::Compute);
    const bool bound = BindResources(&compute);
    ASSERT_MSG(bound, "macOS geometry emulation: binding the {} stage failed",
               stages.hw_stage == Shader::HwStage::Export ? "ES" : "GS");
    if (needs_barrier) {
        runtime.FlushBarriers();
    }
    scheduler.EndRendering();
    compute.BindResources(set_writes, push_data);
    const vk::Pipeline handle = compute.Handle();
    scheduler.Record([=](vk::CommandBuffer cmdbuf) {
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, handle);
        if (indirect) {
            cmdbuf.dispatchIndirect(indirect, indirect_offset);
        } else {
            cmdbuf.dispatch(groups_x, groups_y, 1);
        }
    });
    ResetBindings(true);
    gs_stats.dispatches.fetch_add(1, std::memory_order_relaxed);
}

void Rasterizer::RecordGsBarrier(vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access) {
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
}

namespace {
/// errors[] of gs_indirect_setup.comp: reports, largest scratch dwords asked for, index overruns,
/// guest address (low, high) of the indirect arguments of the last draw reported.
constexpr u32 ErrorDwords = 8;
/// Scratch range of one indirect ES/GS draw (its counts are only known to the GPU) and the ring
/// they come from.
constexpr u64 IndirectScratchBytes = 32_MB;
constexpr u64 IndirectRingBytes = 256_MB;

struct SetupKernel {
    vk::UniqueDescriptorSetLayout set_layout;
    vk::UniquePipelineLayout layout;
    vk::UniquePipeline pipeline;
};

struct SetupParams {
    u32 args_skip, indexed, index_kind, index_skip, index_count;
    u32 verts_per_prim, invocations, max_vert_out, esgs_dwords, vertex_dwords, run_length;
    u32 capacity_dwords;
    u32 args_address_lo, args_address_hi;
    u32 point_output;
};

struct CompactParams {
    u32 invocations, run_length;
    u32 so_buffers;
    std::array<u32, 4> so_size_dw, so_stride_dw;
};

/// A one-binding compute kernel with push descriptors and push constants.
SetupKernel MakeKernel(vk::Device device, std::span<const u32> spv, u32 num_bindings,
                       u32 push_size) {
    SetupKernel k;
    std::array<vk::DescriptorSetLayoutBinding, 3> bindings{};
    for (u32 i = 0; i < num_bindings; ++i) {
        bindings[i] = {.binding = i,
                       .descriptorType = vk::DescriptorType::eStorageBuffer,
                       .descriptorCount = 1,
                       .stageFlags = vk::ShaderStageFlagBits::eCompute};
    }
    auto [layout_result, set_layout] = device.createDescriptorSetLayoutUnique({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = num_bindings,
        .pBindings = bindings.data(),
    });
    ASSERT(layout_result == vk::Result::eSuccess);
    k.set_layout = std::move(set_layout);
    const vk::PushConstantRange range{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                                      .offset = 0,
                                      .size = push_size};
    auto [pipeline_layout_result, pipeline_layout] = device.createPipelineLayoutUnique({
        .setLayoutCount = 1,
        .pSetLayouts = &*k.set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &range,
    });
    ASSERT(pipeline_layout_result == vk::Result::eSuccess);
    k.layout = std::move(pipeline_layout);
    const auto module = CompileSPV(spv, device);
    auto [pipeline_result, compute] = device.createComputePipelineUnique(
        {}, vk::ComputePipelineCreateInfo{
                .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                          .module = module,
                          .pName = "main"},
                .layout = *k.layout,
            });
    ASSERT(pipeline_result == vk::Result::eSuccess);
    k.pipeline = std::move(compute);
    device.destroyShaderModule(module);
    return k;
}
} // namespace

void Rasterizer::RecordGsPointCompaction(const GsEmulationStages& stages,
                                         const AmdGpu::Regs& regs) {
    static const SetupKernel kernel = MakeKernel(instance.GetDevice(), GS_POINT_COMPACT_COMP, 2,
                                                 sizeof(CompactParams));
    CompactParams params{.invocations = stages.invocations, .run_length = stages.run_length};
    // Binding 1: the stream-out state of a stream-out draw (any valid buffer otherwise).
    vk::DescriptorBufferInfo so_info{gs_binding.scratch, gs_binding.scratch_offset,
                                     gs_binding.scratch_size};
    if (stages.so) {
        ASSERT_MSG(so_state, "macOS geometry emulation: stream-out draw before any "
                             "STRMOUT_BUFFER_UPDATE set its buffer offsets");
        params.so_buffers = stages.so_buffers;
        for (u32 buffer = 0; buffer < 4; ++buffer) {
            params.so_size_dw[buffer] = regs.vgt_strmout_buffers[buffer].size_dw;
            params.so_stride_dw[buffer] = regs.vgt_strmout_buffers[buffer].vtx_stride_dw;
        }
        so_info = {so_state->Handle(), 0, SoStateBytes};
    }
    const auto binding = gs_binding;
    scheduler.Record([=](vk::CommandBuffer cmdbuf) {
        const std::array<vk::DescriptorBufferInfo, 2> infos{{
            {binding.scratch, binding.scratch_offset, binding.scratch_size},
            so_info,
        }};
        std::array<vk::WriteDescriptorSet, 2> writes{};
        for (u32 i = 0; i < writes.size(); ++i) {
            writes[i] = {.dstBinding = i,
                         .descriptorCount = 1,
                         .descriptorType = vk::DescriptorType::eStorageBuffer,
                         .pBufferInfo = &infos[i]};
        }
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *kernel.pipeline);
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *kernel.layout, 0, writes);
        cmdbuf.pushConstants(*kernel.layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(params),
                             &params);
        cmdbuf.dispatch(1, 1, 1);
    });
    gs_stats.dispatches.fetch_add(1, std::memory_order_relaxed);
}

void Rasterizer::RecordGsStreamOut(const GsEmulationStages& stages, const AmdGpu::Regs& regs,
                                   vk::Buffer scratch, u64 scratch_offset) {
    // The kernel's stores add the buffer offset as an SGPR offset, which V# bounds do not check
    // on GCN; the compaction tail keeps every write below VGT_STRMOUT_BUFFER_SIZE. Each bound
    // stream-out V# (its range is the binding) must hold that whole buffer, or writes would be
    // dropped (GsEmulation::StreamOutBindingGap).
    const auto& info = stages.so->GetStage(Shader::SwStage::Compute);
    for (const auto& desc : info.buffers) {
        if (desc.stream_out_buffer == Shader::BufferResource::NoStreamOut) {
            continue;
        }
        const u32 buffer = desc.stream_out_buffer;
        const auto& vgt = regs.vgt_strmout_buffers[buffer];
        const auto sharp = desc.GetSharp(info);
        const char* gap = GsEmu::StreamOutBindingGap(sharp, vgt.size_dw, vgt.vtx_stride_dw);
        ASSERT_MSG(!gap,
                   "macOS geometry emulation: {} (copy shader {:#x}, buffer {}: V# at {:#x} "
                   "stride {} x {} records, swizzle {}; VGT size {} dwords, stride {} dwords)",
                   gap ? gap : "", info.pgm_hash, buffer, sharp.base_address, sharp.stride,
                   sharp.num_records, sharp.swizzle_enable, vgt.size_dw, vgt.vtx_stride_dw);
    }
    // After the compaction: SoDispatch, SoCount and SoOffset* are written. The kernel's guest
    // stream-out buffers are bound (and tracked as GPU-written) like any compute pass's.
    RecordGsBarrier(vk::PipelineStageFlagBits2::eComputeShader |
                        vk::PipelineStageFlagBits2::eDrawIndirect,
                    vk::AccessFlagBits2::eShaderStorageRead |
                        vk::AccessFlagBits2::eIndirectCommandRead);
    RecordGsDispatch(*stages.so, 0, 0, scratch, scratch_offset + GsEmu::SoDispatch * 4);
    CheckStreamOutOverflow();
}

void Rasterizer::CheckStreamOutOverflow() {
    const auto* state = reinterpret_cast<const volatile u32*>(so_state->mapped_data.data());
    const u32 draws = state[SoOverflowDraws];
    if (draws != so_overflow_reported) {
        // PS4 behaviour (the vertices that do not fit are dropped), reported, not silent.
        LOG_WARNING(Render_Vulkan,
                    "macOS geometry emulation: {} stream-out draw(s) overflowed their buffers, "
                    "{} vertices dropped in total (as on the PS4)",
                    draws, u32(state[SoOverflowVertices]));
        so_overflow_reported = draws;
    }
}

void Rasterizer::CheckGsEmulationErrors() {
    if (!gs_errors) {
        return;
    }
    const auto* errors = reinterpret_cast<const volatile u32*>(gs_errors->mapped_data.data());
    ASSERT_MSG(errors[0] == 0,
               "macOS geometry emulation: {} indirect ES/GS draw(s) could not be emulated, the "
               "last with arguments at {:#x} (largest scratch need {} dwords of {}; {} read past "
               "the index buffer)",
               errors[0], u64(errors[3]) | (u64(errors[4]) << 32), errors[1],
               IndirectScratchBytes / 4, errors[2]);
}

GsEmulationDraw Rasterizer::RecordGsEmulationIndirect(const GraphicsPipeline* pipeline,
                                                      const AmdGpu::Regs& regs, bool is_indexed,
                                                      VAddr args, u32 stride, u32 max_count,
                                                      VAddr count_address) {
    ASSERT_MSG(count_address == 0 && max_count == 1,
               "macOS geometry emulation: multi-draw indirect ES/GS draw (count at {:#x}, max {})",
               count_address, max_count);
    CheckGsEmulationErrors();
    const auto& stages = *pipeline->GsEmulation();
    const auto device = instance.GetDevice();
    const u64 alignment = instance.StorageMinAlignment();
    static const SetupKernel kernel = MakeKernel(device, GS_INDIRECT_SETUP_COMP, 3,
                                                 sizeof(SetupParams));
    if (!gs_indirect_scratch) {
        gs_indirect_scratch = std::make_unique<VideoCore::StreamBuffer>(
            instance, scheduler, VideoCore::MemoryType::Stream, IndirectRingBytes);
        gs_errors = std::make_unique<VideoCore::Buffer>(instance, 0, ErrorDwords * 4,
                                                        VideoCore::MemoryType::Stream,
                                                        "GsEmulationErrors");
        ASSERT_MSG(!gs_errors->mapped_data.empty(), "macOS geometry emulation: error words unmapped");
        std::memset(gs_errors->mapped_data.data(), 0, ErrorDwords * 4);
    }

    // The guest's indirect command, from its storage-aligned start.
    ASSERT(stride == (is_indexed ? sizeof(VkDrawIndexedIndirectCommand)
                                 : sizeof(VkDrawIndirectCommand)));
    const auto [args_buffer, args_offset] = buffer_cache.ObtainBuffer(args, stride, false);
    needs_barrier |= runtime.IsBufferAccessed(args_buffer, args_offset, stride);
    const u64 args_bind = Common::AlignDown(args_offset, alignment);
    ASSERT_MSG((args_offset - args_bind) % 4 == 0,
               "macOS geometry emulation: indirect arguments at {:#x} are not dword aligned", args);

    // Scratch: the setup kernel writes header and commands; the records follow.
    const auto scratch_offset = gs_indirect_scratch->Reserve(IndirectScratchBytes, alignment);
    ASSERT_MSG(scratch_offset, "macOS geometry emulation: no indirect scratch");
    gs_binding = {.scratch = gs_indirect_scratch->Handle(),
                  .scratch_offset = *scratch_offset,
                  .scratch_size = IndirectScratchBytes,
                  .index = gs_indirect_scratch->Handle(),
                  .index_offset = *scratch_offset,
                  .index_size = IndirectScratchBytes};
    SetupParams params{
        .args_skip = u32((args_offset - args_bind) / 4),
        .indexed = is_indexed ? 1u : 0u,
        .verts_per_prim = stages.verts_per_prim,
        .invocations = stages.invocations,
        .max_vert_out = stages.max_vert_out,
        .esgs_dwords = stages.esgs_dwords,
        .vertex_dwords = stages.vertex_dwords,
        .run_length = stages.run_length,
        .capacity_dwords = u32(IndirectScratchBytes / 4),
        .args_address_lo = u32(args),
        .args_address_hi = u32(args >> 32),
        .point_output = stages.point_output ? 1u : 0u,
    };
    if (is_indexed) {
        ASSERT_MSG((regs.enable_primitive_restart & 1) == 0,
                   "macOS geometry emulation: indexed ES/GS draw with primitive restart");
        ASSERT_MSG(regs.index_buffer_type.swap_mode == AmdGpu::IndexSwapMode::None,
                   "macOS geometry emulation: index swap mode {}",
                   u32(regs.index_buffer_type.swap_mode));
        // The whole guest index buffer (INDEX_BUFFER_SIZE indices); the kernel checks the draw's
        // range against it.
        const bool index16 = regs.index_buffer_type.index_type == AmdGpu::IndexType::Index16;
        const u32 index_size = index16 ? 2 : 4;
        const VAddr address = regs.index_base_address.Address<VAddr>();
        const u32 index_bytes = regs.num_indices * index_size;
        ASSERT_MSG(index_bytes != 0, "macOS geometry emulation: indirect ES/GS draw without an "
                                     "index buffer size");
        const auto [buffer, offset] = buffer_cache.ObtainBuffer(address, index_bytes, false);
        needs_barrier |= runtime.IsBufferAccessed(buffer, offset, index_bytes);
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
        gs_binding.index = buffer->Handle();
        gs_binding.index_offset = bind_offset;
        gs_binding.index_size = bind_size;
        params.index_kind = u32(index16 ? GsEmu::IndexKind::U16 : GsEmu::IndexKind::U32);
        params.index_skip = u32(skip / index_size);
        params.index_count = regs.num_indices;
    }

    gs_stats.draws.fetch_add(1, std::memory_order_relaxed);
    gs_stats.scratch_bytes.fetch_add(IndirectScratchBytes, std::memory_order_relaxed);
    std::optional<RenderState> ended_pass;
    if (scheduler.IsRendering()) {
        ended_pass = scheduler.GetRenderState();
    }
    if (needs_barrier) {
        runtime.FlushBarriers();
    }
    scheduler.EndRendering();
    const vk::Buffer scratch = gs_indirect_scratch->Handle();
    const vk::Buffer errors = gs_errors->Handle();
    scheduler.Record([=, args = args_buffer->Handle(),
                      scratch_offset = *scratch_offset](vk::CommandBuffer cmdbuf) {
        const std::array<vk::DescriptorBufferInfo, 3> infos{{
            {args, args_bind, Common::AlignUp(args_offset - args_bind + stride, 4)},
            {scratch, scratch_offset, IndirectScratchBytes},
            {errors, 0, ErrorDwords * 4},
        }};
        std::array<vk::WriteDescriptorSet, 3> writes{};
        for (u32 i = 0; i < writes.size(); ++i) {
            writes[i] = {.dstBinding = i,
                         .descriptorCount = 1,
                         .descriptorType = vk::DescriptorType::eStorageBuffer,
                         .pBufferInfo = &infos[i]};
        }
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *kernel.pipeline);
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *kernel.layout, 0, writes);
        cmdbuf.pushConstants(*kernel.layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(params),
                             &params);
        cmdbuf.dispatch(1, 1, 1);
    });
    runtime.AccessBuffer(args_buffer, args_offset, stride, vk::PipelineStageFlagBits2::eComputeShader,
                         vk::AccessFlagBits2::eShaderRead);
    RecordGsBarrier(vk::PipelineStageFlagBits2::eDrawIndirect |
                        vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eIndirectCommandRead |
                        vk::AccessFlagBits2::eShaderStorageRead);
    RecordGsDispatch(*stages.es, 0, 0, scratch, *scratch_offset + GsEmu::EsDispatch * 4);
    RecordGsBarrier(vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderStorageRead);
    RecordGsDispatch(*stages.gs, 0, 0, scratch, *scratch_offset + GsEmu::GsDispatch * 4);
    if (stages.point_output) {
        RecordGsBarrier(vk::PipelineStageFlagBits2::eComputeShader,
                        vk::AccessFlagBits2::eShaderStorageRead |
                            vk::AccessFlagBits2::eShaderStorageWrite);
        RecordGsPointCompaction(stages, regs);
        if (stages.so) {
            RecordGsStreamOut(stages, regs, scratch, *scratch_offset);
            if (regs.clipper_control.dx_rasterization_kill) {
                return {.ended_pass = ended_pass, .skip_draw = true};
            }
        }
    }
    RecordGsBarrier(vk::PipelineStageFlagBits2::eVertexShader |
                        vk::PipelineStageFlagBits2::eIndexInput |
                        vk::PipelineStageFlagBits2::eDrawIndirect,
                    vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eIndexRead |
                        vk::AccessFlagBits2::eIndirectCommandRead);
    return {.index_buffer = scratch,
            .index_offset = *scratch_offset,
            .indirect = true,
            .indirect_offset = *scratch_offset + (stages.point_output ? GsEmu::CompactDraw
                                                                      : GsEmu::StripDraw) * 4,
            .ended_pass = ended_pass};
}

} // namespace Vulkan
