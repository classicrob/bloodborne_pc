// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <cstdio>
#include <xxhash.h>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <ranges>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "bbport_toggles.h"
#include "common/hash.h"
#include "common/io_file.h"
#include "common/path_util.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_feature_checks.h"
#include "video_core/renderer_vulkan/motion_history.h"
#include "video_core/renderer_vulkan/vk_draw_prep.h"
#include "video_core/renderer_vulkan/vk_gs_emulation.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_serialization.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan {

using Shader::HwStage;
using Shader::Output;
using Shader::SwStage;

constexpr static auto SpirvVersion1_6 = 0x00010600U;

constexpr static std::array DescriptorHeapSizes = {
    vk::DescriptorPoolSize{vk::DescriptorType::eUniformBuffer, 512},
    vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer, 8192},
    vk::DescriptorPoolSize{vk::DescriptorType::eSampledImage, 8192},
    vk::DescriptorPoolSize{vk::DescriptorType::eStorageImage, 1024},
    vk::DescriptorPoolSize{vk::DescriptorType::eSampler, 1024},
};

static u32 MapOutputs(std::span<Shader::OutputMap, 3> outputs, const AmdGpu::VsOutputControl& ctl) {
    u32 num_outputs = 0;

    if (ctl.vs_out_misc_enable) {
        auto& misc_vec = outputs[num_outputs++];
        misc_vec[0] = ctl.use_vtx_point_size ? Output::PointSize : Output::None;
        misc_vec[1] = ctl.use_vtx_edge_flag
                          ? Output::EdgeFlag
                          : (ctl.use_vtx_gs_cut_flag ? Output::GsCutFlag : Output::None);
        misc_vec[2] =
            ctl.use_vtx_kill_flag
                ? Output::KillFlag
                : (ctl.use_vtx_render_target_idx ? Output::RenderTargetIndex : Output::None);
        misc_vec[3] = ctl.use_vtx_viewport_idx ? Output::ViewportIndex : Output::None;
    }

    if (ctl.vs_out_ccdist0_enable) {
        auto& ccdist0 = outputs[num_outputs++];
        ccdist0[0] = ctl.IsClipDistEnabled(0)
                         ? Output::ClipDist0
                         : (ctl.IsCullDistEnabled(0) ? Output::CullDist0 : Output::None);
        ccdist0[1] = ctl.IsClipDistEnabled(1)
                         ? Output::ClipDist1
                         : (ctl.IsCullDistEnabled(1) ? Output::CullDist1 : Output::None);
        ccdist0[2] = ctl.IsClipDistEnabled(2)
                         ? Output::ClipDist2
                         : (ctl.IsCullDistEnabled(2) ? Output::CullDist2 : Output::None);
        ccdist0[3] = ctl.IsClipDistEnabled(3)
                         ? Output::ClipDist3
                         : (ctl.IsCullDistEnabled(3) ? Output::CullDist3 : Output::None);
    }

    if (ctl.vs_out_ccdist1_enable) {
        auto& ccdist1 = outputs[num_outputs++];
        ccdist1[0] = ctl.IsClipDistEnabled(4)
                         ? Output::ClipDist4
                         : (ctl.IsCullDistEnabled(4) ? Output::CullDist4 : Output::None);
        ccdist1[1] = ctl.IsClipDistEnabled(5)
                         ? Output::ClipDist5
                         : (ctl.IsCullDistEnabled(5) ? Output::CullDist5 : Output::None);
        ccdist1[2] = ctl.IsClipDistEnabled(6)
                         ? Output::ClipDist6
                         : (ctl.IsCullDistEnabled(6) ? Output::CullDist6 : Output::None);
        ccdist1[3] = ctl.IsClipDistEnabled(7)
                         ? Output::ClipDist7
                         : (ctl.IsCullDistEnabled(7) ? Output::CullDist7 : Output::None);
    }

    return num_outputs;
}

const Shader::RuntimeInfo& PipelineCache::BuildRuntimeInfo(PipelineSelection& sel, HwStage stage,
                                                             SwStage l_stage) {
    auto& info = sel.runtime_infos[u32(l_stage)];
    const auto& regs = (*sel.regs);
    const auto BuildCommon = [&](const auto& program) {
        info.props.num_user_data = program.settings.num_user_regs;
        info.props.num_input_vgprs = program.settings.vgpr_comp_cnt;
        info.props.num_allocated_vgprs = program.NumVgprs();
        info.props.fp_denorm_mode32 = program.settings.fp_denorm_mode32;
        info.props.fp_denorm_mode16_64 = program.settings.fp_denorm_mode64;
        info.props.fp_round_mode32 = program.settings.fp_round_mode32;
        info.props.fp_round_mode16_64 = program.settings.fp_round_mode64;
    };
    info.Initialize(stage, l_stage);
    switch (stage) {
    case HwStage::Local: {
        BuildCommon(regs.ls_program);
        Shader::TessellationDataConstantBuffer tess_constants{};
        const auto* hull_info = sel.infos[u32(SwStage::TessellationControl)];
        hull_info->ReadTessConstantBuffer(tess_constants);
        info.hw.ls.ls_stride = tess_constants.ls_stride;
        break;
    }
    case HwStage::Hull:
        BuildCommon(regs.hs_program);
        break;
    case HwStage::Export:
        BuildCommon(regs.es_program);
        info.hw.es.vertex_data_size = regs.vgt_esgs_ring_itemsize;
        if (profile.emulate_geometry_stage) {
            info.gs_role = Shader::GsEmulationRole::EsCompute;
        }
        break;
    case HwStage::Geometry: {
        BuildCommon(regs.gs_program);
        info.hw.gs.num_outputs = MapOutputs(info.hw.gs.outputs, regs.vs_output_control);
        info.hw.gs.output_vertices = regs.vgt_gs_max_vert_out;
        info.hw.gs.num_invocations =
            regs.vgt_gs_instance_cnt.IsEnabled() ? regs.vgt_gs_instance_cnt.count : 1;
        if (regs.stage_enable.raw == AmdGpu::ShaderStageEnable::LsHsEsGs) {
            info.hw.gs.in_primitive = [&]() {
                switch (regs.tess_config.topology) {
                case AmdGpu::TessellationTopology::Point:
                    return AmdGpu::PrimitiveType::PointList;
                case AmdGpu::TessellationTopology::Line:
                    return AmdGpu::PrimitiveType::LineList;
                case AmdGpu::TessellationTopology::TriangleCw:
                case AmdGpu::TessellationTopology::TriangleCcw:
                    return AmdGpu::PrimitiveType::TriangleList;
                default:
                    UNREACHABLE();
                }
            }();
        } else {
            info.hw.gs.in_primitive = regs.primitive_type;
        }
        for (u32 stream_id = 0; stream_id < Shader::GsMaxOutputStreams; ++stream_id) {
            info.hw.gs.out_primitive[stream_id] =
                regs.vgt_gs_out_prim_type.GetPrimitiveType(stream_id);
        }
        info.hw.gs.in_vertex_data_size = regs.vgt_esgs_ring_itemsize;
        info.hw.gs.out_vertex_data_size = regs.vgt_gs_vert_itemsize[0];
        info.hw.gs.mode = regs.vgt_gs_mode.mode;
        if (profile.emulate_geometry_stage) {
            info.gs_role = Shader::GsEmulationRole::GsCompute;
            info.gs_max_vert_out = regs.vgt_gs_max_vert_out;
            info.gs_vertex_dwords = regs.vgt_gs_vert_itemsize[0];
        }
        const auto params_vc = AmdGpu::GetParams(regs.vs_program);
        info.hw.gs.vs_copy = params_vc.code;
        info.hw.gs.vs_copy_hash = params_vc.hash;
        DumpShader(info.hw.gs.vs_copy, info.hw.gs.vs_copy_hash, Shader::HwStage::Vertex, 0,
                   "copy.bin");
        break;
    }
    case HwStage::Vertex: {
        BuildCommon(regs.vs_program);
        info.hw.vs.user_clip_plane_mask = regs.clipper_control.user_clip_plane_enable;
        info.hw.vs.num_outputs = MapOutputs(info.hw.vs.outputs, regs.vs_output_control);
        info.hw.vs.emulate_depth_negative_one_to_one =
            !instance.IsDepthClipControlSupported() &&
            regs.clipper_control.clip_space == AmdGpu::ClipSpace::MinusWToW;
        info.hw.vs.clip_disable = regs.IsClipDisabled();
        info.hw.vs.motion_vectors = sel.motion;
        if (profile.emulate_geometry_stage &&
            regs.stage_enable.raw == AmdGpu::ShaderStageEnable::VgtStages::EsGs) {
            // The hardware VS of an ES/GS draw is the copy shader.
            info.gs_role = Shader::GsEmulationRole::CopyVertex;
            info.gs_max_vert_out = regs.vgt_gs_max_vert_out;
            info.gs_vertex_dwords = regs.vgt_gs_vert_itemsize[0];
            // Rasterized GS points take PA_SU_POINT_SIZE (Vulkan would otherwise use 1 px).
            if (Shader::GsEmulation::PointOutput(regs.vgt_gs_out_prim_type.GetPrimitiveType(0)) &&
                !regs.clipper_control.dx_rasterization_kill) {
                const auto& size = regs.point_size;
                ASSERT_MSG(!regs.vs_output_control.use_vtx_point_size,
                           "macOS geometry emulation: GS points with an exported point size "
                           "(PA_SU_POINT_MINMAX clamp {:#x} not emulated)",
                           std::bit_cast<u32>(regs.point_minmax));
                ASSERT_MSG(size.half_width == size.half_height && size.half_width != 0,
                           "macOS geometry emulation: GS points of {}x{} (half sizes, 12.4): "
                           "Vulkan points are square and not empty",
                           u32(size.half_width), u32(size.half_height));
                info.hw.vs.point_size = float(size.half_width) / 8.0f;
                ASSERT_MSG(info.hw.vs.point_size <= instance.GetMaxPointSize(),
                           "macOS geometry emulation: GS points of {} px exceed the device's "
                           "{} px maximum",
                           info.hw.vs.point_size, instance.GetMaxPointSize());
            }
        }
        break;
    }
    case HwStage::Fragment: {
        BuildCommon(regs.ps_program);
        info.hw.fs.en_flags = regs.ps_input_ena;
        info.hw.fs.addr_flags = regs.ps_input_addr;
        info.hw.fs.num_inputs = regs.num_interp;
        info.hw.fs.front_face_all_bits = regs.barycentric_control.front_face_all_bits;
        info.hw.fs.num_samples =
            regs.ps_input_addr.sample_coverage_ena && regs.ps_input_ena.sample_coverage_ena
                ? regs.aa_config.NumSamples()
                : 1;
        info.hw.fs.z_export_format = regs.z_export_format;
        info.hw.fs.motion_vectors = sel.motion;
#ifdef __APPLE__
        // The device has no depth bounds test: the fragment shader emulates it against the
        // stored depth whenever skipping it would change the result (docs/macos/depth-bounds.md).
        info.hw.fs.depth_bounds_test = FeatureChecks::DepthBoundsUnsupported(
            regs.depth_control.depth_bounds_enable, regs.depth_buffer.DepthValid(),
            regs.depth_bounds_min, regs.depth_bounds_max, instance.IsDepthBoundsSupported());
#endif
        u8 stencil_ref_export_enable = regs.depth_shader_control.stencil_op_val_export_enable |
                                       regs.depth_shader_control.stencil_test_val_export_enable;
        info.hw.fs.mrtz_mask = regs.depth_shader_control.z_export_enable |
                               (stencil_ref_export_enable << 1) |
                               (regs.depth_shader_control.mask_export_enable << 2) |
                               (regs.depth_shader_control.coverage_to_mask_enable << 3);
        const auto& cb0_blend = regs.blend_control[0];
        if (cb0_blend.enable) {
            info.hw.fs.dual_source_blending =
                LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.color_dst_factor) ||
                LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.color_src_factor);
            if (cb0_blend.separate_alpha_blend) {
                info.hw.fs.dual_source_blending |=
                    LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.alpha_dst_factor) ||
                    LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.alpha_src_factor);
            }
        } else {
            info.hw.fs.dual_source_blending = false;
        }
        const auto& ps_inputs = regs.ps_inputs;
        for (u32 i = 0; i < regs.num_interp; i++) {
            info.hw.fs.inputs[i] = {
                .param_index = u8(ps_inputs[i].input_offset),
                .is_default = bool(ps_inputs[i].use_default),
                .is_flat = bool(ps_inputs[i].flat_shade),
                .default_value = u8(ps_inputs[i].default_value),
            };
        }
        for (u32 i = 0; i < Shader::MaxColorBuffers; i++) {
            info.hw.fs.color_buffers[i] = sel.graphics_key.color_buffers[i];
        }
        // Lowered user clip planes ride the same emulation path as guest-exported distances, so
        // the fragment side arms whenever the hardware vertex stage lowers them, keeping its input
        // locations in sync with the shifted vertex outputs.
        const bool lowers_user_clip_planes =
            regs.clipper_control.user_clip_plane_enable &&
            !regs.stage_enable.IsStageEnabled(static_cast<u32>(HwStage::Geometry));
        info.hw.fs.clip_distance_emulation =
            ((regs.vs_output_control.clip_distance_enable &&
              !regs.stage_enable.IsStageEnabled(static_cast<u32>(HwStage::Local))) ||
             lowers_user_clip_planes) &&
            profile.needs_clip_distance_emulation;
        break;
    }
    case HwStage::Compute: {
        const auto& cs_pgm = liverpool->GetCsRegs();
        info.props.num_user_data = cs_pgm.settings.num_user_regs;
        info.props.num_allocated_vgprs = cs_pgm.settings.num_vgprs * 4;
        info.props.fp_denorm_mode32 = cs_pgm.settings.fp_denorm_mode32;
        info.props.fp_denorm_mode16_64 = cs_pgm.settings.fp_denorm_mode64;
        info.props.fp_round_mode32 = cs_pgm.settings.fp_round_mode32;
        info.props.fp_round_mode16_64 = cs_pgm.settings.fp_round_mode64;
        info.hw.cs.workgroup_size = {cs_pgm.num_thread_x.full, cs_pgm.num_thread_y.full,
                                     cs_pgm.num_thread_z.full};
        info.hw.cs.tgid_enable = {cs_pgm.IsTgidEnabled(0), cs_pgm.IsTgidEnabled(1),
                                  cs_pgm.IsTgidEnabled(2)};
        info.hw.cs.shared_memory_size = cs_pgm.SharedMemSize();
        break;
    }
    default:
        break;
    }
    switch (l_stage) {
    case SwStage::Vertex:
        info.sw.vs.step_rate_0 = regs.vgt_instance_step_rate_0;
        info.sw.vs.step_rate_1 = regs.vgt_instance_step_rate_1;
        info.sw.vs.vertex_sgpr_offset = sel.draw_indirect_params.vertex_sgpr_offset;
        info.sw.vs.instance_sgpr_offset = sel.draw_indirect_params.instance_sgpr_offset;
        info.sw.vs.tess_emulated_primitive =
            regs.primitive_type == AmdGpu::PrimitiveType::RectList ||
            regs.primitive_type == AmdGpu::PrimitiveType::QuadList;
        break;
    case SwStage::TessellationControl: {
        info.sw.tcs.num_input_control_points = regs.ls_hs_config.hs_input_control_points;
        info.sw.tcs.num_threads = regs.ls_hs_config.hs_output_control_points;
        info.sw.tcs.tess_type = regs.tess_config.type;
        info.sw.tcs.offchip_lds_enable = regs.hs_program.settings.oc_lds_en;
        break;
    }
    case SwStage::TessellationEval: {
        info.sw.tes.tess_type = regs.tess_config.type;
        info.sw.tes.tess_topology = regs.tess_config.topology;
        info.sw.tes.tess_partitioning = regs.tess_config.partitioning;
        break;
    }
    default:
        break;
    }
    return info;
}

PipelineCache::PipelineCache(const Instance& instance_, Scheduler& scheduler_,
                             AmdGpu::Liverpool* liverpool_, u32 sparse_page_shift)
    : instance{instance_}, scheduler{scheduler_}, liverpool{liverpool_},
      desc_heap{instance, scheduler.GetWorkSemaphore(), DescriptorHeapSizes} {
    sel.regs = &liverpool->regs;
    const auto& vk12_props = instance.GetVk12Properties();
    profile = Shader::Profile{
        .max_viewport_width = instance.GetMaxViewportWidth(),
        .max_viewport_height = instance.GetMaxViewportHeight(),
        .max_shared_memory_size = instance.MaxComputeSharedMemorySize(),
        .supported_spirv = SpirvVersion1_6,
        .subgroup_size = instance.SubgroupSize(),
        .subgroup_supported_stages = instance.SubgroupSupportedStages(),
        .sparse_page_shift = sparse_page_shift,
        .support_int8 = instance.IsShaderInt8Supported(),
        .support_int16 = instance.IsShaderInt16Supported(),
        .support_int64 = instance.IsShaderInt64Supported(),
        .support_float16 = instance.IsShaderFloat16Supported(),
        .support_float64 = instance.IsShaderFloat64Supported(),
        .supports_denorm_behavior_independence =
            vk12_props.denormBehaviorIndependence != vk::ShaderFloatControlsIndependence::eNone,
        .supports_rounding_mode_independence =
            vk12_props.roundingModeIndependence != vk::ShaderFloatControlsIndependence::eNone,
        .support_fp16_denorm_preserve = bool(vk12_props.shaderDenormPreserveFloat16),
        .support_fp16_denorm_flush = bool(vk12_props.shaderDenormFlushToZeroFloat16),
        .support_fp16_round_to_zero = bool(vk12_props.shaderRoundingModeRTZFloat16),
        .support_fp32_denorm_preserve = bool(vk12_props.shaderDenormPreserveFloat32),
        .support_fp32_denorm_flush = bool(vk12_props.shaderDenormFlushToZeroFloat32),
        .support_fp32_round_to_zero = bool(vk12_props.shaderRoundingModeRTZFloat32),
        .support_fp64_denorm_preserve = bool(vk12_props.shaderDenormPreserveFloat64),
        .support_fp64_denorm_flush = bool(vk12_props.shaderDenormFlushToZeroFloat64),
        .support_fp64_round_to_zero = bool(vk12_props.shaderRoundingModeRTZFloat64),
        .support_fp16_signed_zero_inf_nan_preserve =
            bool(vk12_props.shaderSignedZeroInfNanPreserveFloat16),
        .support_fp32_signed_zero_inf_nan_preserve =
            bool(vk12_props.shaderSignedZeroInfNanPreserveFloat32),
        .support_fp64_signed_zero_inf_nan_preserve =
            bool(vk12_props.shaderSignedZeroInfNanPreserveFloat64),
        .supports_image_load_store_lod = instance_.IsImageLoadStoreLodSupported(),
        .supports_native_cube_calc = instance_.IsAmdGcnShaderSupported(),
        .supports_trinary_minmax = instance_.IsAmdShaderTrinaryMinMaxSupported(),
        .supports_buffer_fp32_atomic_min_max =
            instance_.IsShaderAtomicFloatBuffer32MinMaxSupported(),
        .supports_image_fp32_atomic_min_max = instance_.IsShaderAtomicFloatImage32MinMaxSupported(),
        .supports_buffer_int64_atomics = instance_.IsBufferInt64AtomicsSupported(),
        .supports_shared_int64_atomics = instance_.IsSharedInt64AtomicsSupported(),
        .supports_workgroup_explicit_memory_layout =
            instance_.IsWorkgroupMemoryExplicitLayoutSupported(),
        .supports_amd_shader_explicit_vertex_parameter =
            instance_.IsAmdShaderExplicitVertexParameterSupported(),
        .supports_fragment_shader_barycentric = instance_.IsFragmentShaderBarycentricSupported(),
#ifdef __APPLE__
        // MoltenVK reports barycentrics but SPIRV-Cross rejects PerVertexKHR inputs
        // (spirv_msl.cpp, "PerVertexKHR decoration is not supported in MSL").
        .supports_fragment_per_vertex_inputs = false,
#endif
        .supports_shader_subgroup_clock = instance_.IsShaderSubgroupClockSupported(),
        .needs_manual_interpolation = instance.IsFragmentShaderBarycentricSupported() &&
                                      instance.GetDriverID() == vk::DriverId::eNvidiaProprietary,
        // bbport: older NVIDIA (Pascal) has no barycentrics; BB_INTERP_INT_FIX=0/1 overrides.
        .needs_integer_interpolation_fix = [&] {
            if (const char* env = std::getenv("BB_INTERP_INT_FIX")) {
                return env[0] == '1';
            }
            return !instance.IsFragmentShaderBarycentricSupported() &&
                   instance.GetDriverID() == vk::DriverId::eNvidiaProprietary;
        }(),
        .needs_lds_barriers = instance.GetDriverID() == vk::DriverId::eNvidiaProprietary ||
                              instance.GetDriverID() == vk::DriverId::eMesaKosmickrisp,
        .needs_buffer_offsets = instance.StorageMinAlignment() > 4,
        .needs_unorm_fixup = instance.GetDriverID() == vk::DriverId::eMesaKosmickrisp,
        .needs_clip_distance_emulation = instance.GetDriverID() == vk::DriverId::eNvidiaProprietary,
        .supports_shader_stencil_export = instance_.IsShaderStencilExportSupported(),
    };
    profile.emulate_null_descriptors = instance.EmulatesNullDescriptors();
#ifdef __APPLE__
    profile.emulate_robust_buffer_access = true;
    profile.emulate_vertex_fetch = true;
    profile.emulate_geometry_stage = true;
    profile.repair_mul_signed_zero = true;
    // 32-wide subgroups: GCN wave64 semantics are emulated (lower_wave64_pass.cpp).
    profile.emulate_wave64 = true;
#endif
    WarmUp();

    auto [cache_result, cache] = instance.GetDevice().createPipelineCacheUnique({});
    ASSERT_MSG(cache_result == vk::Result::eSuccess, "Failed to create pipeline cache: {}",
               vk::to_string(cache_result));
    pipeline_cache = std::move(cache);
#ifdef __APPLE__
    // New graphics pipelines are built on workers (docs/macos/pipeline-compile.md).
    build_queue = std::make_unique<PipelineBuildQueue>(
        std::clamp<u32>(std::thread::hardware_concurrency(), 1, 8));
#endif
}

PipelineCache::~PipelineCache() = default;

// bbport: shader/pipeline compile time on the GPU thread, reported by BB_FRAME_STATS.
std::atomic<u64> g_bb_compile_ns;
std::atomic<u32> g_bb_compiles;
/// The part of the above that translated guest shaders to SPIR-V; the rest created pipelines
/// (on Apple: MoltenVK's SPIR-V to MSL conversion and the Metal compile).
std::atomic<u64> g_bb_translate_ns;
std::atomic<u32> g_bb_translations;
namespace {
struct CompileTimer {
    const char* what;
    u64 hash;
    bool translation = false;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    ~CompileTimer() {
        const u64 ns = u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now() - start)
                               .count());
        g_bb_compile_ns += ns;
        ++g_bb_compiles;
        if (translation) {
            g_bb_translate_ns += ns;
            ++g_bb_translations;
        }
        if (ns >= 50'000'000) {
            LOG_INFO(Render_Vulkan, "Slow compile: {} {:#x} took {:.1f} ms", what, hash, ns / 1e6);
        }
    }
};
} // namespace

bool PipelineCache::PrepareGraphicsPipeline(PipelineSelection& worker_sel) {
#ifdef __APPLE__
    // BB_PIPELINE_VERIFY checks selections made on the GPU thread only.
    if (PipelineVerify()) {
        return false;
    }
#endif
    // Tessellation stages read constant buffers from memory at selection time: not prepared.
    if (worker_sel.regs->stage_enable.hs_en) {
        return false;
    }
#ifdef __APPLE__
    // Emulated geometry stages select three programs and compute pipelines: not prepared.
    if (worker_sel.regs->stage_enable.gs_en) {
        return false;
    }
#endif
    return RefreshGraphicsKey(worker_sel) && !worker_sel.worker->failed;
}

const GraphicsPipeline* PipelineCache::TryPreparedPipeline(const PreparedDraw& prepared) {
    if (prepared.state.load(std::memory_order_acquire) != PreparedDraw::Ready ||
        prepared.reg_checksum != liverpool->gfx_reg_checksum) {
        return nullptr;
    }
    // Same registers; the stage programs and their flattened user data (which also covers
    // everything read from guest memory for the specialization) must match as well. This is
    // the per-stage work GetProgram does on the regular path.
    const auto& regs = liverpool->regs;
    for (u32 i = 0; i < prepared.num_stages; ++i) {
        const auto& stage = prepared.stages[i];
        const auto* pgm = regs.ProgramForStage(static_cast<u32>(stage.hw_stage));
        if (!pgm || !pgm->Address<u32*>()) {
            return nullptr;
        }
        const auto params = AmdGpu::GetParams(*pgm);
        if (params.hash != stage.hash) {
            return nullptr;
        }
        auto& info = *const_cast<Shader::Info*>(stage.info);
        info.pgm_base = params.Base();
        info.user_data = params.user_data;
        if (info.has_fetch_shader) {
            const auto module = std::ranges::find_if(stage.program->modules, [&](const auto& entry) {
                return entry.info.get() == stage.info;
            });
            if (module == stage.program->modules.end() ||
                Shader::Gcn::ParseFetchShader(info) != module->spec.fetch_shader_data) {
                return nullptr;
            }
        }
        info.RefreshFlatBuf();
        if (info.pgm_base != stage.pgm_base || info.flattened_ud_buf.size() != stage.flat_size ||
            std::memcmp(info.flattened_ud_buf.data(), stage.flat,
                        stage.flat_size * sizeof(u32)) != 0) {
            return nullptr;
        }
    }
    const auto it = graphics_pipelines.find(prepared.key);
    return it != graphics_pipelines.end() ? it->second.get() : nullptr;
}

const GraphicsPipeline* PipelineCache::GetGraphicsPipeline(const DrawIndirectParams params,
                                                           const PreparedDraw* prepared) {
    used_prepared = nullptr;
#ifdef __APPLE__
    // BB_PIPELINE_VERIFY: prepared selections skip GetProgram and the key path; take those.
    if (prepared && PipelineVerify()) {
        ++verify_prepared_bypassed;
        prepared = nullptr;
    }
#endif
    if (prepared) {
        if (const auto* pipeline = TryPreparedPipeline(*prepared)) {
            used_prepared = prepared;
            return pipeline;
        }
    }
    sel.draw_indirect_params = params;
    sel.refusal = DrawDrop::StageCombination;
    if (!RefreshGraphicsKey(sel)) {
        CountDrawDrop(sel.refusal);
        return nullptr;
    }
    const auto [it, is_new] = graphics_pipelines.try_emplace(sel.graphics_key);
#ifdef __APPLE__
    if (!is_new && it->second && PipelineVerify()) {
        VerifyReusedPipeline(*it->second, std::hash<GraphicsPipelineKey>{}(sel.graphics_key));
    }
#endif
    if (is_new) {
        const auto pipeline_hash = std::hash<GraphicsPipelineKey>{}(sel.graphics_key);
        LOG_INFO(Render_Vulkan, "Compiling graphics pipeline {:#x}", pipeline_hash);

        GraphicsPipeline::SerializationSupport sdata{};
        const bool on_worker = build_queue && !BbToggle::Disabled(BbToggle::PipelineWorkers);
        if (on_worker) {
            // Built on a worker (posted below); the draw recording thread waits for the handle.
            it.value() = std::make_unique<GraphicsPipeline>(
                instance, scheduler, desc_heap, profile, sel.graphics_key, *pipeline_cache,
                sel.infos, sel.runtime_infos, sel.fetch_shader, sel.modules, sdata, false, true);
        } else {
            CompileTimer timer{"graphics pipeline", pipeline_hash};
            it.value() = std::make_unique<GraphicsPipeline>(
                instance, scheduler, desc_heap, profile, sel.graphics_key, *pipeline_cache,
                sel.infos, sel.runtime_infos, sel.fetch_shader, sel.modules, sdata, false);
        }

        if (sel.graphics_key.gs_es_hash) {
            // Emulated geometry stage: the ES and GS compute pipelines. Not stored in the pipeline
            // cache (its records cannot rebuild them); compiled again in each session.
            const auto& regs = *sel.regs;
            const auto compute = [&](const PipelineSelection::GsEmulationProgram& program) {
                // Shared by every graphics pipeline using this ES/GS permutation.
                auto& shared = gs_compute_pipelines[program.hash];
                if (!shared) {
                    ComputePipeline::SerializationSupport compute_sdata{};
                    shared = std::make_unique<ComputePipeline>(
                        instance, scheduler, desc_heap, profile, *pipeline_cache,
                        ComputePipelineKey{program.hash}, *program.info, program.module,
                        compute_sdata, false);
                }
                return static_cast<const ComputePipeline*>(shared.get());
            };
            auto stages = std::make_unique<GsEmulationStages>();
            stages->es = compute(sel.gs_es);
            stages->gs = compute(sel.gs_gs);
            stages->verts_per_prim = GsEmulationVerticesPerPrimitive(regs);
            stages->invocations = 1;
            stages->max_vert_out = regs.vgt_gs_max_vert_out;
            stages->esgs_dwords = regs.vgt_esgs_ring_itemsize;
            stages->vertex_dwords = regs.vgt_gs_vert_itemsize[0];
            stages->run_length = Shader::GsEmulation::IndexRunLength(
                stages->max_vert_out, regs.vgt_gs_out_prim_type.GetPrimitiveType(0));
            stages->point_output = Shader::GsEmulation::PointOutput(
                regs.vgt_gs_out_prim_type.GetPrimitiveType(0));
            it.value()->SetGsEmulation(std::move(stages));
        } else if (!on_worker) {
            RegisterPipelineData(sel.graphics_key, pipeline_hash, sdata);
        }
        if (on_worker) {
            // GS pipelines are not recorded either way (see above).
            build_queue->Post([pipeline = it.value().get(), key = sel.graphics_key, pipeline_hash,
                               record = sel.graphics_key.gs_es_hash == 0] {
                CompileTimer timer{"graphics pipeline", pipeline_hash};
                auto& built_sdata = pipeline->BuildDeferred();
                if (record) {
                    RegisterPipelineData(key, pipeline_hash, built_sdata);
                }
            });
        }
        ++num_new_pipelines;

        if (EmulatorSettings.IsShaderCollect()) {
            for (auto stage = 0; stage < MaxShaderStages; ++stage) {
                if (sel.infos[stage]) {
                    auto& m = sel.modules[stage];
                    module_related_pipelines[m].emplace_back(sel.graphics_key);
                }
            }
        }
        sel.fetch_shader.reset();
    }
    return it->second.get();
}

const ComputePipeline* PipelineCache::GetComputePipeline() {
    if (!RefreshComputeKey()) {
        return nullptr;
    }
    const auto [it, is_new] = compute_pipelines.try_emplace(compute_key);
    if (is_new) {
        const auto pipeline_hash = std::hash<ComputePipelineKey>{}(compute_key);
        LOG_INFO(Render_Vulkan, "Compiling compute pipeline {:#x}", pipeline_hash);
        CompileTimer timer{"compute pipeline", pipeline_hash};

        ComputePipeline::SerializationSupport sdata{};
        it.value() = std::make_unique<ComputePipeline>(instance, scheduler, desc_heap, profile,
                                                       *pipeline_cache, compute_key, *sel.infos[0],
                                                       sel.modules[0], sdata, false);
        RegisterPipelineData(compute_key, sdata);
        ++num_new_pipelines;

        if (EmulatorSettings.IsShaderCollect()) {
            auto& m = sel.modules[0];
            module_related_pipelines[m].emplace_back(compute_key);
        }
    }
    return it->second.get();
}

bool PipelineCache::RefreshGraphicsKey(PipelineSelection& sel) {
    std::memset(&sel.graphics_key, 0, sizeof(GraphicsPipelineKey));
    const auto& regs = (*sel.regs);
    auto& key = sel.graphics_key;

    // bbport: the pipeline declares exactly the depth/stencil attachments the render pass binds.
    // The rasterizer binds the depth target only when the depth or stencil test is enabled
    // (Rasterizer::PrepareRenderState); with both off a valid depth buffer is not attached, and a
    // pipeline still declaring its format does not match the pass (Vulkan dynamic rendering;
    // Metal validation: "the renderPipelineState pixelFormat must be MTLPixelFormatInvalid").
    // With both tests off the guest neither tests nor writes depth or stencil, so this is exact.
    const bool db_bound =
        (regs.depth_control.depth_enable && regs.depth_buffer.DepthValid()) ||
        (regs.depth_control.stencil_enable && regs.depth_buffer.StencilValid());
    const bool db_enabled =
        db_bound && (regs.depth_buffer.DepthValid() || regs.depth_buffer.StencilValid());

    key.z_format = db_bound && regs.depth_buffer.DepthValid()
                       ? regs.depth_buffer.z_info.format
                       : AmdGpu::DepthBuffer::ZFormat::Invalid;
    key.stencil_format = db_bound && regs.depth_buffer.StencilValid()
                             ? regs.depth_buffer.stencil_info.format
                             : AmdGpu::DepthBuffer::StencilFormat::Invalid;
    key.depth_clamp_enable = !regs.depth_render_override.disable_viewport_clamp;
    key.depth_clip_enable = regs.clipper_control.ZclipEnable();
    key.clip_space = regs.clipper_control.clip_space;
    key.provoking_vtx_last = regs.polygon_control.provoking_vtx_last;
    key.prim_type = regs.primitive_type;
#ifdef __APPLE__
    if (profile.emulate_geometry_stage &&
        regs.stage_enable.raw == AmdGpu::ShaderStageEnable::VgtStages::EsGs) {
        // The emulated draw rasterizes the GS output strips, or its points (vk_gs_emulation.h).
        switch (regs.vgt_gs_out_prim_type.GetPrimitiveType(0)) {
        case AmdGpu::GsOutputPrimitiveType::LineStrip:
            key.prim_type = AmdGpu::PrimitiveType::LineStrip;
            break;
        case AmdGpu::GsOutputPrimitiveType::PointList:
            key.prim_type = AmdGpu::PrimitiveType::PointList;
            break;
        default:
            key.prim_type = AmdGpu::PrimitiveType::TriangleStrip;
            break;
        }
    }
    // One Vulkan polygon mode: the visible faces' (vk_feature_checks.h; mismatches are errors).
    {
        const auto& poly = regs.polygon_control;
        const bool gs = regs.stage_enable.IsStageEnabled(u32(Shader::HwStage::Geometry));
        const bool tess = regs.stage_enable.IsStageEnabled(u32(Shader::HwStage::Hull));
        const auto raster = FeatureChecks::RasterizedPrimitive(
            regs.primitive_type, gs, regs.vgt_gs_out_prim_type.GetPrimitiveType(0), tess,
            regs.tess_config.topology);
        const bool culled = FeatureChecks::FacesCullable(
            raster, gs || tess, LiverpoolToVK::IsPrimitiveCulled(regs.primitive_type));
        key.polygon_mode = FeatureChecks::VisiblePolygonMode(
            poly.enable_polygon_mode != 0, poly.polygon_mode_front, poly.polygon_mode_back,
            culled && poly.cull_front, culled && poly.cull_back);
    }
#else
    key.polygon_mode = regs.polygon_control.PolyMode();
#endif
    key.patch_control_points =
        regs.stage_enable.hs_en ? regs.ls_hs_config.hs_input_control_points : 0;
    key.logic_op = regs.color_control.rop3;
    key.depth_samples = db_enabled ? regs.depth_buffer.NumSamples() : 1;
    key.num_samples = key.depth_samples;
    key.cb_shader_mask = regs.color_shader_mask;

    const bool skip_cb_binding =
        regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable;

    // Only potentially animated G-buffer draws may use the extra motion attachment. The
    // ordinary stage variant is needed first to inspect the vertex shader's resources.
    bool motion_possible = false;
    {
        u32 bound = 0;
        for (s32 cb = 0; cb < AmdGpu::NUM_COLOR_BUFFERS && !skip_cb_binding; ++cb) {
            bound += (regs.color_buffers[cb] && regs.color_target_mask.GetMask(cb)) ? 1 : 0;
        }
        motion_possible = Shader::MotionVectors::positions_address != 0 && bound >= 5 &&
                          !regs.color_buffers[Shader::MotionVectors::Output] &&
                          regs.depth_buffer.DepthValid() &&
                          regs.depth_buffer.NumSamples() == 1 && !regs.IsClipDisabled() &&
                          regs.stage_enable.raw == AmdGpu::ShaderStageEnable::VgtStages::Vs;
    }
    sel.motion = false;

    // First pass to fill render target information needed by shader recompiler
    for (s32 cb = 0; cb < AmdGpu::NUM_COLOR_BUFFERS && !skip_cb_binding; ++cb) {
        const auto& col_buf = regs.color_buffers[cb];
        if (!col_buf || !regs.color_target_mask.GetMask(cb)) {
            // No attachment bound or writing to it is disabled.
            continue;
        }

        // Fill color target information
        auto& color_buffer = key.color_buffers[cb];
        color_buffer.data_format = col_buf.GetDataFmt();
        color_buffer.num_format = col_buf.GetNumberFmt();
        color_buffer.num_conversion = col_buf.GetNumberConversion();
        color_buffer.export_format = regs.color_export_format.GetFormat(cb);
        color_buffer.swizzle = col_buf.Swizzle();

        const auto& bc = regs.blend_control[cb];
        color_buffer.blend_self_scale =
            bc.enable && !col_buf.info.blend_bypass &&
            (bc.color_func == AmdGpu::BlendControl::BlendFunc::Min ||
             bc.color_func == AmdGpu::BlendControl::BlendFunc::Max) &&
            bc.color_src_factor == AmdGpu::BlendControl::BlendFactor::SrcColor &&
            bc.color_dst_factor == AmdGpu::BlendControl::BlendFactor::DstColor;
    }

    // Compile and bind shader stages
    if (!RefreshGraphicsStages(sel)) {
        return false;
    }
    if (motion_possible) {
        const auto* vs = sel.infos[static_cast<u32>(Shader::SwStage::Vertex)];
        if (vs) {
            // Shaders with a bone palette (motion_history.h). Small skeletons (weapons,
            // props) also include static world pieces: the rasterizer gives those
            // history only while their constants change.
            for (const auto& resource : vs->buffers) {
                if (resource.IsSpecial()) continue;
                const auto buffer = resource.GetSharp(*vs);
                if (buffer.Valid() && buffer.GetStride() == 16 &&
                    Motion::ClassifyBuffer(buffer.GetSize()) != Motion::BufferRole::Other) {
                    sel.motion = true;
                    break;
                }
            }
        }
        // BB_MOTION_SELECT_LOG=1: each G-buffer vertex shader once, with its buffer sizes
        // and the selection, to find animated models that the size rule leaves out.
        static const bool select_log = [] {
            const char* value = std::getenv("BB_MOTION_SELECT_LOG");
            return value && value[0] == '1';
        }();
        if (select_log && vs) {
            std::string sizes;
            for (const auto& resource : vs->buffers) {
                if (resource.IsSpecial()) continue;
                const auto buffer = resource.GetSharp(*vs);
                sizes += fmt::format(" {}/{}", buffer.GetSize(), buffer.GetStride());
            }
            static std::mutex log_mutex;
            static std::unordered_set<u64> logged;
            std::scoped_lock lock{log_mutex};
            if (logged.insert(vs->pgm_hash ^ std::hash<std::string>{}(sizes)).second) {
                std::printf("Motion select: vs %016llx %s, buffers (size/stride):%s\n",
                            static_cast<unsigned long long>(vs->pgm_hash),
                            sel.motion ? "ON " : "off", sizes.c_str());
            }
        }
        // Keep the old broad path available for visual A/B tests.
        static const bool all_motion = [] {
            const char* value = std::getenv("BB_OBJECT_MOTION_ALL");
            return value && value[0] == '1';
        }();
        if (all_motion) {
            sel.motion = true;
        }
        if (sel.motion && !RefreshGraphicsStages(sel)) {
            return false;
        }
    }

    // Second pass to mask out render targets not written by shader and fill remaining info
    u8 color_samples = 0;
    bool all_color_samples_same = true;
    for (s32 cb = 0; cb < key.num_color_attachments && !skip_cb_binding; ++cb) {
        const auto& col_buf = regs.color_buffers[cb];
        const u32 target_mask = regs.color_target_mask.GetMask(cb);
        if (!col_buf || !target_mask) {
            continue;
        }
        if ((key.mrt_mask & (1u << cb)) == 0) {
            std::memset(&key.color_buffers[cb], 0, sizeof(Shader::PsColorBuffer));
            continue;
        }

        // Fill color blending information
        if (regs.blend_control[cb].enable && !col_buf.info.blend_bypass) {
            key.blend_controls[cb] = regs.blend_control[cb];
        }

        // Apply swizzle to target mask
        key.write_masks[cb] =
            vk::ColorComponentFlags{key.color_buffers[cb].swizzle.ApplyMask(target_mask)};

        // Fill color samples
        const u8 prev_color_samples = std::exchange(color_samples, col_buf.NumSamples());
        all_color_samples_same &= color_samples == prev_color_samples || prev_color_samples == 0;
        key.color_samples[cb] = color_samples;
        key.num_samples = std::max(key.num_samples, color_samples);
    }

    if (sel.motion) {
        constexpr u32 mv = Shader::MotionVectors::Output;
        // Targets the fragment shader does not write are not attached (PrepareRenderState). The
        // pass above cleared them only below the shader's last output; widening the pipeline to
        // the motion slot must clear the rest too, or the pipeline declares color attachments the
        // pass never binds (Metal validation: "For color attachment 6, the renderPipelineState
        // pixelFormat must be MTLPixelFormatInvalid"; the pipeline's per-sample tile storage then
        // differs from the render pass's).
        for (u32 cb = key.num_color_attachments; cb < mv; ++cb) {
            std::memset(&key.color_buffers[cb], 0, sizeof(Shader::PsColorBuffer));
            key.blend_controls[cb] = {};
            key.write_masks[cb] = {};
            key.color_samples[cb] = 0;
        }
        key.motion_vectors = 1;
        key.mrt_mask |= 1u << mv;
        key.num_color_attachments = mv + 1;
        auto& color_buffer = key.color_buffers[mv];
        color_buffer.data_format = AmdGpu::DataFormat::Format32_32_32_32;
        color_buffer.num_format = AmdGpu::NumberFormat::Float;
        color_buffer.swizzle = AmdGpu::IdentityMapping;
        key.write_masks[mv] = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                              vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
        key.color_samples[mv] = 1;
    }

    // Force all color samples to match depth samples to avoid unsupported MSAA configuration
    if (color_samples != 0) {
        const bool depth_mismatch = db_enabled && color_samples != key.depth_samples;
        if (!all_color_samples_same && !instance.IsMixedAnySamplesSupported() ||
            all_color_samples_same && depth_mismatch && !instance.IsMixedDepthSamplesSupported()) {
            key.color_samples.fill(key.depth_samples);
            key.num_samples = key.depth_samples;
        }
    }

    return true;
}

#ifdef __APPLE__
namespace {
/// Apple has no geometry stage; vk_gs_emulation.h covers ES/GS draws except `gap`. Reports such a
/// draw's whole ES/GS configuration: the GS registers in the log, and the ES, GS and copy-shader
/// binaries plus their user data under <shader dir>/gs_report. Stops the game, unless
/// BB_GS_CENSUS=1 asks to record every distinct configuration and not draw them (a diagnostic run,
/// which then renders incompletely and logs how many draws it dropped).
bool ReportGeometryStages(const AmdGpu::Regs& regs, const char* gap) {
    static const bool census = [] {
        const char* value = std::getenv("BB_GS_CENSUS");
        return value && value[0] == '1';
    }();
    const auto es = AmdGpu::GetParams(regs.es_program);
    const auto gs = AmdGpu::GetParams(regs.gs_program);
    const auto vs = AmdGpu::GetParams(regs.vs_program);
    const u32 gs_mode = std::bit_cast<u32>(regs.vgt_gs_mode);
    const u32 onchip = std::bit_cast<u32>(regs.vgt_gs_onchip_control);
    const auto key = fmt::format(
        "stages {:#x} es {:#018x} gs {:#018x} copy {:#018x} prim {} gs_mode {:#x} onchip {:#x} "
        "out_prim {:#x} max_vert_out {} instance_cnt {:#x} esgs_itemsize {} gsvs_itemsize {} "
        "vert_itemsize {}/{}/{}/{} gsvs_offsets {}/{}/{} strmout {:#x} vs_output_control {:#x}",
        static_cast<u32>(regs.stage_enable.raw), es.hash, gs.hash, vs.hash,
        static_cast<u32>(regs.primitive_type), gs_mode, onchip, regs.vgt_gs_out_prim_type.raw,
        u32(regs.vgt_gs_max_vert_out), regs.vgt_gs_instance_cnt.raw, regs.vgt_esgs_ring_itemsize,
        regs.vgt_gsvs_ring_itemsize, regs.vgt_gs_vert_itemsize[0], regs.vgt_gs_vert_itemsize[1],
        regs.vgt_gs_vert_itemsize[2], regs.vgt_gs_vert_itemsize[3],
        regs.vgt_gsvs_ring_offset_1.raw, regs.vgt_gsvs_ring_offset_2.raw,
        regs.vgt_gsvs_ring_offset_3.raw, regs.vgt_strmout_config.raw,
        std::bit_cast<u32>(regs.vs_output_control));

    static std::mutex mutex;
    static std::unordered_map<std::string, u64> seen;
    u64 count;
    {
        std::scoped_lock lock{mutex};
        count = ++seen[key];
    }
    if (count == 1) {
        using namespace Common::FS;
        const auto dir = GetUserPath(PathType::ShaderDir) / "gs_report";
        std::filesystem::create_directories(dir);
        const auto dump = [&](const char* stage, const Shader::ShaderParams& params) {
            const auto name = fmt::format("{}_{:#018x}", stage, params.hash);
            IOFile{dir / (name + ".bin"), FileAccessMode::Create}.WriteSpan(params.code);
            IOFile{dir / (name + ".ud"), FileAccessMode::Create}.WriteSpan(
                std::span<const u32>{params.user_data});
        };
        dump("es", es);
        dump("gs", gs);
        dump("copy", vs);
        LOG_ERROR(Render_Vulkan, "Geometry configuration: {} (instances {}, indices {}; not "
                  "emulated: {})",
                  key, regs.num_instances.NumInstances(), regs.num_indices, gap);
    } else if (std::has_single_bit(count)) {
        LOG_ERROR(Render_Vulkan, "Geometry configuration drawn {} times: {}", count, key);
    }
    if (!census) {
        UNREACHABLE_MSG("macOS geometry shader emulation does not cover {}: {}", gap, key);
    }
    return false;
}
} // Anonymous namespace
#endif

bool PipelineCache::RefreshGraphicsStages(PipelineSelection& sel) {
    const auto& regs = (*sel.regs);
    auto& key = sel.graphics_key;
    sel.fetch_shader = std::nullopt;

    Shader::Backend::Bindings binding{};
    const auto bind_stage = [&](HwStage stage_in, SwStage stage_out) -> bool {
        const auto stage_in_idx = static_cast<u32>(stage_in);
        const auto stage_out_idx = static_cast<u32>(stage_out);
        if (!regs.stage_enable.IsStageEnabled(stage_in_idx)) {
            key.stage_hashes[stage_out_idx] = 0;
            sel.infos[stage_out_idx] = nullptr;
            sel.refusal = DrawDrop::StageDisabled;
            return false;
        }

        const auto* pgm = regs.ProgramForStage(stage_in_idx);
        if (!pgm || !pgm->Address<u32*>()) {
            key.stage_hashes[stage_out_idx] = 0;
            sel.infos[stage_out_idx] = nullptr;
            sel.refusal = DrawDrop::NullProgram;
            return false;
        }

        const auto params = AmdGpu::GetParams(*pgm);
        std::optional<Shader::Gcn::FetchShaderData> fetch_shader_;
        std::tie(sel.infos[stage_out_idx], sel.modules[stage_out_idx], fetch_shader_,
                 key.stage_hashes[stage_out_idx]) =
            GetProgram(sel, stage_in, stage_out, params, binding);
        if (fetch_shader_) {
            sel.fetch_shader = fetch_shader_;
        }
        return true;
    };

    sel.infos.fill(nullptr);
    sel.modules.fill(nullptr);
    sel.gs_es = {};
    sel.gs_gs = {};

    bind_stage(HwStage::Fragment, SwStage::Fragment);

    const auto* fs_info = sel.infos[static_cast<u32>(SwStage::Fragment)];
    key.mrt_mask = fs_info ? fs_info->mrt_mask : 0u;
    key.num_color_attachments = std::bit_width(key.mrt_mask);

    switch (regs.stage_enable.raw) {
    case AmdGpu::ShaderStageEnable::VgtStages::EsGs:
#ifdef __APPLE__
        // No geometry stage: the ES and GS become compute pipelines (owned by the graphics
        // pipeline, vk_gs_emulation.h) and the copy shader is the draw's vertex shader.
        if (const char* gap = GsEmulationUnsupported(regs)) {
            sel.refusal = DrawDrop::GsReported;
            return ReportGeometryStages(regs, gap);
        }
        {
            const auto bind_compute = [&](HwStage stage_in, SwStage stage_out,
                                          PipelineSelection::GsEmulationProgram& out) {
                const auto* pgm = regs.ProgramForStage(static_cast<u32>(stage_in));
                ASSERT_MSG(pgm && pgm->Address<u32*>(),
                           "macOS geometry emulation: ES/GS draw without a{} program",
                           stage_in == HwStage::Export ? "n ES" : " GS");
                // Each compute pipeline has its own layout: bindings start at 0.
                Shader::Backend::Bindings compute_binding{};
                const auto program =
                    GetProgram(sel, stage_in, stage_out, AmdGpu::GetParams(*pgm), compute_binding);
                out = {std::get<0>(program), std::get<1>(program), std::get<3>(program)};
                ASSERT_MSG(out.info, "macOS geometry emulation: no {} program selected",
                           stage_in == HwStage::Export ? "ES" : "GS");
                return true;
            };
            if (!bind_compute(HwStage::Export, SwStage::Vertex, sel.gs_es) ||
                !bind_compute(HwStage::Geometry, SwStage::Geometry, sel.gs_gs)) {
                return false;
            }
            key.gs_es_hash = sel.gs_es.hash;
            key.gs_gs_hash = sel.gs_gs.hash;
            if (!bind_stage(HwStage::Vertex, SwStage::Vertex)) {
                return false;
            }
        }
        break;
#endif
        if (!instance.IsGeometryStageSupported()) {
            LOG_WARNING(Render_Vulkan, "Geometry shader stage unsupported, skipping");
            return false;
        }
        if (regs.vgt_gs_mode.onchip || regs.vgt_strmout_config.raw) {
#ifdef __APPLE__
            UNREACHABLE_MSG("macOS on-chip geometry shaders or stream-out are not implemented");
#endif
            LOG_WARNING(Render_Vulkan, "Geometry shader features unsupported, skipping");
            return false;
        }
        if (!bind_stage(HwStage::Export, SwStage::Vertex)) {
            return false;
        }
        if (!bind_stage(HwStage::Geometry, SwStage::Geometry)) {
            return false;
        }
        break;
    case AmdGpu::ShaderStageEnable::VgtStages::LsHs:
        if (!instance.IsTessellationSupported()) {
#ifdef __APPLE__
            UNREACHABLE_MSG("macOS device cannot execute required tessellation shaders");
#endif
            return false;
        }
        if (!bind_stage(HwStage::Hull, SwStage::TessellationControl)) {
            return false;
        }
        if (!bind_stage(HwStage::Vertex, SwStage::TessellationEval)) {
            return false;
        }
        if (!bind_stage(HwStage::Local, SwStage::Vertex)) {
            return false;
        }
        break;
    case AmdGpu::ShaderStageEnable::VgtStages::LsHsEsGs:
        if (!instance.IsTessellationSupported()) {
#ifdef __APPLE__
            UNREACHABLE_MSG("macOS device cannot execute required tessellation shaders");
#endif
            return false;
        }
        if (!instance.IsGeometryStageSupported()) {
#ifdef __APPLE__
            sel.refusal = DrawDrop::GsReported;
            return ReportGeometryStages(regs, "tessellation feeding a geometry shader (LS/HS/ES/GS)");
#endif
            LOG_WARNING(Render_Vulkan, "Geometry shader stage unsupported, skipping");
            return false;
        }
        if (regs.vgt_gs_mode.onchip || regs.vgt_strmout_config.raw) {
#ifdef __APPLE__
            UNREACHABLE_MSG("macOS on-chip geometry shaders or stream-out are not implemented");
#endif
            LOG_WARNING(Render_Vulkan, "Geometry shader features unsupported, skipping");
            return false;
        }
        if (!bind_stage(HwStage::Hull, SwStage::TessellationControl)) {
            return false;
        }
        if (!bind_stage(HwStage::Export, SwStage::TessellationEval)) {
            return false;
        }
        if (!bind_stage(HwStage::Local, SwStage::Vertex)) {
            return false;
        }
        if (!bind_stage(HwStage::Geometry, SwStage::Geometry)) {
            return false;
        }
        break;
    case AmdGpu::ShaderStageEnable::VgtStages::Vs:
        bind_stage(HwStage::Vertex, SwStage::Vertex);
        break;
    default:
        LOG_WARNING(Render_Vulkan, "unimplemented shader stage {}", (u32)regs.stage_enable.raw);
        sel.refusal = DrawDrop::StageCombination;
        return false;
    }

    const auto* vs_info = sel.infos[static_cast<u32>(SwStage::Vertex)];
    if (!profile.emulate_vertex_fetch && vs_info && sel.fetch_shader &&
        !instance.IsVertexInputDynamicState()) {
        // Without vertex input dynamic state, the pipeline needs to specialize on format.
        // Stride will still be handled outside the pipeline using dynamic state.
        u32 vertex_binding = 0;
        for (const auto& attrib : sel.fetch_shader->attributes) {
            const auto& buffer = attrib.GetSharp(*vs_info);
            ASSERT_MSG(vertex_binding < MaxVertexBufferCount,
                       "Vertex attribute binding count exceeded limit: {} >= {}", vertex_binding,
                       MaxVertexBufferCount);
            key.vertex_buffer_formats[vertex_binding++] =
                Vulkan::LiverpoolToVK::SurfaceFormat(buffer.GetDataFmt(), buffer.GetNumberFmt());
        }
    }

    return true;
}

bool PipelineCache::RefreshComputeKey() {
    Shader::Backend::Bindings binding{};
    const auto& cs_pgm = liverpool->GetCsRegs();
    const auto cs_params = AmdGpu::GetParams(cs_pgm);
    std::tie(sel.infos[0], sel.modules[0], sel.fetch_shader, compute_key.value) =
        GetProgram(sel, HwStage::Compute, SwStage::Compute, cs_params, binding);
    return true;
}

vk::ShaderModule PipelineCache::CompileModule(Shader::Info& info, Shader::RuntimeInfo& runtime_info,
                                              const std::span<const u32>& code, size_t perm_idx,
                                              Shader::Backend::Bindings& binding) {
    LOG_INFO(Render_Vulkan, "Compiling {} shader {:#x} {}", info.hw_stage, info.pgm_hash,
             perm_idx != 0 ? "(permutation)" : "");
    DumpShader(code, info.pgm_hash, info.hw_stage, perm_idx, "bin");
    CompileTimer timer{"shader translation", info.pgm_hash, true};

    const auto ir_program = Shader::TranslateProgram(code, pools, info, runtime_info, profile);
    auto spv = Shader::Backend::SPIRV::EmitSPIRV(profile, runtime_info, ir_program, binding);
    DumpShader(spv, info.pgm_hash, info.hw_stage, perm_idx, "spv");

    vk::ShaderModule module;

    auto patch = GetShaderPatch(info.pgm_hash, info.hw_stage, perm_idx, "spv");
    const bool is_patched = patch && EmulatorSettings.IsPatchShaders();
    if (is_patched) {
        LOG_INFO(Loader, "Loaded patch for {} shader {:#x}", info.hw_stage, info.pgm_hash);
        module = CompileSPV(*patch, instance.GetDevice());
    } else {
        module = CompileSPV(spv, instance.GetDevice());
    }

    RegisterShaderBinary(std::move(spv), info.pgm_hash, perm_idx);

    const auto name = GetShaderName(info.hw_stage, info.pgm_hash, perm_idx);
    Vulkan::SetObjectName(instance.GetDevice(), module, name);
    if (EmulatorSettings.IsShaderCollect()) {
        DebugState.CollectShader(name, info.sw_stage, module, spv, code,
                                 patch ? *patch : std::span<const u32>{}, is_patched);
    }
    return module;
}

PipelineCache::Result PipelineCache::GetProgram(PipelineSelection& sel, HwStage hw_stage,
                                                SwStage sw_stage,
                                                const Shader::ShaderParams& params,
                                                Shader::Backend::Bindings& binding) {
    auto runtime_info = BuildRuntimeInfo(sel, hw_stage, sw_stage);
    const auto same_role = [&](const Program::Module& module) {
        // GS and copy VS can share a program hash. Reject incompatible stages/roles before
        // touching their live data: a previously selected module may still be used by this draw.
        // Full specialization compares derived tessellation constants after RefreshFlatBuf.
        // They are not populated in BuildRuntimeInfo, so only stage identity is valid here.
        const auto& stored = module.spec.runtime_info;
        return stored.hw_stage == runtime_info.hw_stage &&
               stored.sw_stage == runtime_info.sw_stage && stored.gs_role == runtime_info.gs_role;
    };
    // Reuse staging storage without copying the module's resource metadata. The existing
    // thread-local snapshot accessors give specialization a private view of this draw.
    const u32 saved_snapshots = Shader::Info::num_ud_snapshots;
    ASSERT(saved_snapshots < Shader::Info::MaxUdSnapshots);
    static thread_local std::vector<u32> candidate_flat;
    const auto matches_data = [&](const Shader::Info& info, const Program::Module& module) {
        auto& view = Shader::Info::ud_snapshots[saved_snapshots];
        view = {&info, params.user_data.data(), u32(params.user_data.size()), nullptr, 0,
                params.Base()};
        Shader::Info::num_ud_snapshots = saved_snapshots + 1;
        bool matches = false;
        try {
            // A different fetch program can walk different tables. Reject it before the walker.
            if (!info.has_fetch_shader ||
                Shader::Gcn::ParseFetchShader(info) == module.spec.fetch_shader_data) {
                info.RefreshFlatBuf(params.user_data, candidate_flat);
                view.flat = candidate_flat.data();
                view.flat_size = u32(candidate_flat.size());
                matches = module.spec ==
                    Shader::StageSpecialization(info, runtime_info, profile, binding);
            }
        } catch (...) {
            Shader::Info::num_ud_snapshots = saved_snapshots;
            throw;
        }
        Shader::Info::num_ud_snapshots = saved_snapshots;
        return matches;
    };
    const auto publish = [&](Shader::Info& destination) {
        // Resource metadata and its address remain stable for cached pipelines. Rejected
        // candidates never change live data, even temporarily. Recycle the previous flat buffer.
        destination.pgm_base = params.Base();
        destination.user_data = params.user_data;
        destination.flattened_ud_buf.swap(candidate_flat);
    };
    if (sel.worker) {
        auto& worker = *sel.worker;
        std::shared_lock lk{programs_mutex};
        const auto found = program_cache.find(params.hash);
        if (found == program_cache.end()) {
            worker.failed = true;
            return {};
        }
        const Program* program = found->second.get();
        for (size_t index = 0; index < program->modules.size(); ++index) {
            const auto& module = program->modules[index];
            if (!module.info || !module.info_template || !same_role(module)) {
                continue;
            }
            sigjmp_buf recover;
            if (sigsetjmp(recover, 0)) {
                Shader::Info::num_ud_snapshots = saved_snapshots;
                runtime_fault_recover = nullptr;
                worker.failed = true;
                return {};
            }
            runtime_fault_recover = &recover;
            if (!matches_data(*module.info_template, module)) {
                runtime_fault_recover = nullptr;
                continue;
            }
            runtime_fault_recover = nullptr;
            auto it = worker.infos.try_emplace(module.info.get(), *module.info_template).first;
            auto& info = it->second;
            publish(info);
            info.AddBindings(binding);
            worker.stages.push_back({program, module.info.get(), params.hash, hw_stage,
                                     info.pgm_base, &info.flattened_ud_buf});
            return {&info, module.module, module.spec.fetch_shader_data,
                    HashCombine(params.hash, index)};
        }
        worker.failed = true;
        return {};
    }

    auto found = program_cache.find(params.hash);
    if (found == program_cache.end()) {
        auto program = std::make_unique<Program>();
        std::unique_lock lk{programs_mutex};
        found = program_cache.emplace(params.hash, std::move(program)).first;
    }
    auto& program = *found->second;
    const auto matches = [&](size_t index) {
        auto& module = program.modules[index];
        if (!module.info || !same_role(module)) {
            return false;
        }
        if (!matches_data(*module.info, module)) {
            return false;
        }
        publish(*module.info);
        return true;
    };
    size_t index = program.modules.size();
    if (program.last_used < program.modules.size() && matches(program.last_used)) {
        index = program.last_used;
    } else {
        for (size_t candidate = 0; candidate < program.modules.size(); ++candidate) {
            if (candidate != program.last_used && matches(candidate)) {
                index = candidate;
                break;
            }
        }
    }
    if (index == program.modules.size()) {
        const auto start = binding;
        Shader::Info info(hw_stage, sw_stage, params);
        const auto module = CompileModule(info, runtime_info, params.code, index, binding);
        auto spec = Shader::StageSpecialization(info, runtime_info, profile, start);
        RegisterShaderMeta(info, spec.fetch_shader_data, spec, HashCombine(params.hash, index), index);
        std::unique_lock lk{programs_mutex};
        program.AddPermut(module, std::move(spec), std::move(info));
    } else {
#ifdef __APPLE__
        if (auto& reused = program.modules[index]; reused.preloaded && !reused.verified &&
                                                  PipelineVerify()) {
            reused.verified = true;
            VerifyPreloadedModule(reused, hw_stage, sw_stage, params, runtime_info, binding);
        }
#endif
        program.modules[index].info->AddBindings(binding);
    }
    program.last_used = index;
    const auto& selected = program.modules[index];
    return {selected.info.get(), selected.module, selected.spec.fetch_shader_data,
            HashCombine(params.hash, index)};
}

#ifdef __APPLE__
bool PipelineCache::PipelineVerify() {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_PIPELINE_VERIFY");
        const bool on = env && env[0] == '1';
        if (on) {
            std::fprintf(stderr, "BB_PIPELINE_VERIFY=1: preloaded shader permutations are "
                                 "translated again at first use and compared with the cache; "
                                 "reused graphics pipelines are compared with the draw "
                                 "(diagnostic)\n");
        }
        return on;
    }();
    return enabled;
}

void PipelineCache::VerifyReusedPipeline(const GraphicsPipeline& pipeline, u64 pipeline_hash) {
    // A pipeline is reused for every draw with its key; its construction read these inputs. If
    // the draw's differ, the key does not cover something construction depends on.
    static constexpr const char* names[] = {"FS", "TCS", "TES", "VS", "GS", "CS"}; // SwStage
    std::string differences;
    for (u32 stage = 0; stage < MaxShaderStages; ++stage) {
        if (!pipeline.GetStages()[stage] && !sel.infos[stage]) {
            continue;
        }
        const char* name = stage < std::size(names) ? names[stage] : "stage";
        if (pipeline.built_modules[stage] != sel.modules[stage]) {
            differences += fmt::format("{}{} module", differences.empty() ? "" : ", ", name);
        }
        // A preloaded pipeline's tessellation stages carry the specialization's runtime info,
        // with the fields it derives from the tess constants; the draw's has none of those.
        const bool tess = stage == u32(Shader::SwStage::TessellationControl) ||
                          stage == u32(Shader::SwStage::TessellationEval);
        if (pipeline.built_preloaded && tess) {
            ++verify_tess_unchecked; // not compared: see verify summary
        } else if (!(pipeline.built_runtime_infos[stage] == sel.runtime_infos[stage])) {
            differences += fmt::format("{}{} runtime info", differences.empty() ? "" : ", ", name);
        }
    }
    ++verify_pipelines;
    if (!differences.empty()) {
        ++verify_pipeline_mismatches; // before the summary, which may print for this reuse
    }
    if ((verify_pipelines & 4095) == 0) {
        LOG_INFO(Render_Vulkan,
                 "PIPELINE-VERIFY summary: {} pipeline reuses checked, {} differ; {} preloaded "
                 "modules checked, {} differ; {} preloaded tessellation-stage runtime infos not "
                 "compared; {} prepared selections bypassed for the check",
                 verify_pipelines, verify_pipeline_mismatches, verify_modules,
                 verify_module_mismatches, verify_tess_unchecked, verify_prepared_bypassed);
    }
    if (differences.empty()) {
        return;
    }
    if (verify_pipeline_mismatches <= 64 || (verify_pipeline_mismatches & 1023) == 0) {
        LOG_CRITICAL(Render_Vulkan,
                     "PIPELINE-VERIFY: {} pipeline {:#x} reused for a draw that differs in: {} "
                     "({} of {} reuses differ so far)",
                     pipeline.built_preloaded ? "preloaded" : "live", pipeline_hash, differences,
                     verify_pipeline_mismatches, verify_pipelines);
    }
}

void PipelineCache::VerifyPreloadedModule(const Program::Module& module, HwStage hw_stage,
                                          SwStage sw_stage, const Shader::ShaderParams& params,
                                          const Shader::RuntimeInfo& runtime_info,
                                          const Shader::Backend::Bindings& start) {
    // The same translation CompileModule does, without creating or storing anything.
    Shader::Info fresh(hw_stage, sw_stage, params);
    auto fresh_runtime = runtime_info;
    auto bindings = start;
    const auto ir_program = Shader::TranslateProgram(params.code, pools, fresh, fresh_runtime, profile);
    const auto spv = Shader::Backend::SPIRV::EmitSPIRV(profile, fresh_runtime, ir_program, bindings);
    const u64 fresh_hash = XXH3_64bits(spv.data(), spv.size() * sizeof(u32));
    // As CompileModule: the specialization takes the runtime info as translation left it.
    const auto fresh_spec = Shader::StageSpecialization(fresh, fresh_runtime, profile, start);
    const auto& cached = *module.info;
    std::string differences;
    const auto differ = [&](const char* what) {
        differences += differences.empty() ? what : fmt::format(", {}", what);
    };
    if (fresh_hash != module.binary_hash) differ("SPIR-V");
    if (!(fresh_spec == module.spec)) differ("specialization");
    if (fresh.buffers.size() != cached.buffers.size()) differ("buffer count");
    for (size_t i = 0; i < std::min(fresh.buffers.size(), cached.buffers.size()); ++i) {
        const auto& a = fresh.buffers[i];
        const auto& b = cached.buffers[i];
        if (!(a.sharp_fetch == b.sharp_fetch) || a.buffer_type != b.buffer_type ||
            a.is_written != b.is_written || a.is_formatted != b.is_formatted ||
            a.used_types != b.used_types || a.post_op != b.post_op ||
            a.post_op_dw1_mask != b.post_op_dw1_mask ||
            a.required_binding_bytes != b.required_binding_bytes) {
            differ(fmt::format("buffer {}", i).c_str());
        }
    }
    if (fresh.images.size() != cached.images.size()) differ("image count");
    for (size_t i = 0; i < std::min(fresh.images.size(), cached.images.size()); ++i) {
        const auto& a = fresh.images[i];
        const auto& b = cached.images[i];
        // The fresh Info has no permutation binding count yet: its specialization does.
        const u32 fresh_bindings =
            i < fresh_spec.images.size() ? fresh_spec.images[i].num_bindings : 0;
        if (!(a.sharp_fetch == b.sharp_fetch) || a.is_depth != b.is_depth ||
            a.is_atomic != b.is_atomic || a.is_array != b.is_array ||
            a.is_written != b.is_written || a.is_r128 != b.is_r128 ||
            a.needs_native != b.needs_native || a.constant_mip_index != b.constant_mip_index ||
            a.mip_fallback_mode != b.mip_fallback_mode || a.post_op != b.post_op ||
            fresh_bindings != b.permutation_bindings) {
            differ(fmt::format("image {}", i).c_str());
        }
    }
    if (fresh.samplers.size() != cached.samplers.size()) differ("sampler count");
    for (size_t i = 0; i < std::min(fresh.samplers.size(), cached.samplers.size()); ++i) {
        const auto& a = fresh.samplers[i];
        const auto& b = cached.samplers[i];
        if (!(a.sharp_fetch == b.sharp_fetch) || a.post_op != b.post_op ||
            a.post_op_tsharp_dw3_off != b.post_op_tsharp_dw3_off || a.is_depth != b.is_depth) {
            differ(fmt::format("sampler {}", i).c_str());
        }
    }
    if (fresh.fmasks.size() != cached.fmasks.size()) differ("fmask count");
    for (size_t i = 0; i < std::min(fresh.fmasks.size(), cached.fmasks.size()); ++i) {
        if (fresh.fmasks[i].sharp_idx != cached.fmasks[i].sharp_idx) {
            differ(fmt::format("fmask {}", i).c_str());
        }
    }
    if (fresh.ud_mask.mask != cached.ud_mask.mask) differ("user-data mask");
    if (fresh.mrt_mask != cached.mrt_mask) differ("MRT mask");
    if (fresh.has_fetch_shader != cached.has_fetch_shader ||
        fresh.fetch_shader_sgpr_base != cached.fetch_shader_sgpr_base) {
        differ("fetch shader");
    }
    if (fresh.shared_memory_scratch_size != cached.shared_memory_scratch_size) differ("scratch");
    if (fresh.uses_dma != cached.uses_dma) differ("DMA");
    if (std::memcmp(fresh.loads.flags.data(), cached.loads.flags.data(),
                    sizeof(fresh.loads.flags)) != 0 ||
        std::memcmp(fresh.stores.flags.data(), cached.stores.flags.data(),
                    sizeof(fresh.stores.flags)) != 0) {
        differ("attribute loads/stores");
    }
    for (u32 i = 0; i < fresh.fs_interpolation.size(); ++i) {
        if (fresh.fs_interpolation[i].primary != cached.fs_interpolation[i].primary ||
            fresh.fs_interpolation[i].auxiliary != cached.fs_interpolation[i].auxiliary) {
            differ("FS interpolation");
            break;
        }
    }
    if (fresh.srt_info.flattened_bufsize_dw != cached.srt_info.flattened_bufsize_dw) {
        differ("flattened user data size");
    }
    ++verify_modules;
    if (differences.empty()) {
        return;
    }
    ++verify_module_mismatches;
    LOG_CRITICAL(Render_Vulkan,
                 "PIPELINE-VERIFY: preloaded {} shader {:#x} differs from a fresh translation: {} "
                 "(cached SPIR-V {:#018x}, fresh {:#018x}; {} of {} preloaded modules differ so far)",
                 hw_stage, params.hash, differences, module.binary_hash, fresh_hash,
                 verify_module_mismatches, verify_modules);
}
#endif

std::optional<vk::ShaderModule> PipelineCache::ReplaceShader(vk::ShaderModule module,
                                                             std::span<const u32> spv_code) {
    // bbport: queued builds hold raw pipeline pointers and module handles, and a worker records
    // the pipeline after SetBuilt: let every build finish before destroying or erasing any.
    if (build_queue) {
        build_queue->WaitIdle();
    }
    std::optional<vk::ShaderModule> new_module{};
    for (const auto& [_, program] : program_cache) {
        for (auto& m : program->modules) {
            if (m.module == module) {
                const auto& d = instance.GetDevice();
                d.destroyShaderModule(m.module);
                m.module = CompileSPV(spv_code, d);
                new_module = m.module;
            }
        }
    }
    if (module_related_pipelines.contains(module)) {
        auto& pipeline_keys = module_related_pipelines[module];
        for (auto& key : pipeline_keys) {
            if (std::holds_alternative<GraphicsPipelineKey>(key)) {
                auto& graphics_key = std::get<GraphicsPipelineKey>(key);
                graphics_pipelines.erase(graphics_key);
            } else if (std::holds_alternative<ComputePipelineKey>(key)) {
                auto& compute_key = std::get<ComputePipelineKey>(key);
                compute_pipelines.erase(compute_key);
            }
        }
    }
    return new_module;
}

std::string PipelineCache::GetShaderName(Shader::HwStage stage, u64 hash,
                                         std::optional<size_t> perm) {
    if (perm) {
        return fmt::format("{}_{:#018x}_{}", stage, hash, *perm);
    }
    return fmt::format("{}_{:#018x}", stage, hash);
}

void PipelineCache::DumpShader(std::span<const u32> code, u64 hash, Shader::HwStage stage,
                               size_t perm_idx, std::string_view ext) {
    if (!EmulatorSettings.IsDumpShaders()) {
        return;
    }

    using namespace Common::FS;
    const auto dump_dir = GetUserPath(PathType::ShaderDir) / "dumps";
    if (!std::filesystem::exists(dump_dir)) {
        std::filesystem::create_directories(dump_dir);
    }
    const auto filename = fmt::format("{}.{}", GetShaderName(stage, hash, perm_idx), ext);
    const auto file = IOFile{dump_dir / filename, FileAccessMode::Create};
    file.WriteSpan(code);
}

std::optional<std::vector<u32>> PipelineCache::GetShaderPatch(u64 hash, Shader::HwStage stage,
                                                              size_t perm_idx,
                                                              std::string_view ext) {

    using namespace Common::FS;
    const auto patch_dir = GetUserPath(PathType::ShaderDir) / "patch";
    if (!std::filesystem::exists(patch_dir)) {
        std::filesystem::create_directories(patch_dir);
    }
    const auto filename = fmt::format("{}.{}", GetShaderName(stage, hash, perm_idx), ext);
    const auto filepath = patch_dir / filename;
    if (!std::filesystem::exists(filepath)) {
        return {};
    }
    const auto file = IOFile{patch_dir / filename, FileAccessMode::Read};
    std::vector<u32> code(file.GetSize() / sizeof(u32));
    file.Read(code);
    return code;
}
} // namespace Vulkan
