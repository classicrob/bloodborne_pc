// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <boost/container/static_vector.hpp>
#include <xxhash.h>

#include "shader_recompiler/frontend/fetch_shader.h"
#include "video_core/amdgpu/regs_color.h"
#include "video_core/amdgpu/regs_depth.h"
#include "video_core/amdgpu/regs_primitive.h"
#include "video_core/renderer_vulkan/vk_pipeline_common.h"

namespace VideoCore {
class BufferCache;
class TextureCache;
} // namespace VideoCore

namespace Vulkan {

static constexpr u32 MaxShaderStages = static_cast<u32>(Shader::SwStage::NumLogicalStages);
static constexpr u32 MaxVertexBufferCount = 32;

class Instance;
class Scheduler;
class DescriptorHeap;

template <typename T>
using VertexInputs = boost::container::static_vector<T, MaxVertexBufferCount>;

struct GraphicsPipelineKey {
    std::array<size_t, MaxShaderStages> stage_hashes;
    std::array<vk::Format, MaxVertexBufferCount> vertex_buffer_formats;
    u32 patch_control_points;
    u32 num_color_attachments;
    std::array<Shader::PsColorBuffer, AmdGpu::NUM_COLOR_BUFFERS> color_buffers;
    std::array<AmdGpu::BlendControl, AmdGpu::NUM_COLOR_BUFFERS> blend_controls;
    std::array<vk::ColorComponentFlags, AmdGpu::NUM_COLOR_BUFFERS> write_masks;
    AmdGpu::ColorBufferMask cb_shader_mask;
    AmdGpu::ColorControl::LogicOp logic_op;
    u8 num_samples;
    u8 depth_samples;
    std::array<u8, AmdGpu::NUM_COLOR_BUFFERS> color_samples;
    u32 mrt_mask;
    u32 motion_vectors = 0;
    /// bbport (Apple): ES and GS of an emulated geometry stage (compute roles); 0 otherwise. The
    /// vertex stage of such a pipeline is the copy shader (vk_gs_emulation.h).
    size_t gs_es_hash;
    size_t gs_gs_hash;
    size_t gs_so_hash; ///< the copy shader's stream-out kernel (stream-out draws); 0 otherwise
    struct {
        AmdGpu::DepthBuffer::ZFormat z_format : 2;
        AmdGpu::DepthBuffer::StencilFormat stencil_format : 1;
        u32 depth_clamp_enable : 1;
    };
    struct {
        AmdGpu::PrimitiveType prim_type : 5;
        AmdGpu::PolygonMode polygon_mode : 2;
        AmdGpu::ClipSpace clip_space : 1;
        AmdGpu::ProvokingVtxLast provoking_vtx_last : 1;
        u32 depth_clip_enable : 1;
    };

    GraphicsPipelineKey() {
        std::memset(this, 0, sizeof(*this));
    }

    bool operator==(const GraphicsPipelineKey& key) const noexcept {
        return std::memcmp(this, &key, sizeof(key)) == 0;
    }

    void Serialize(Serialization::Archive& ar) const;
    bool Deserialize(Serialization::Archive& ar);
};

struct GsEmulationStages;

/// The fragment slot of a pipeline's runtime infos is read whether or not it has a fragment
/// shader (depth-bounds layout, clip-distance emulation). Without one it must be the default:
/// true when that holds.
[[nodiscard]] inline bool FragmentInfoMatchesShader(
    std::span<const Shader::RuntimeInfo, MaxShaderStages> runtime_infos,
    std::span<const vk::ShaderModule> modules) {
    const u32 fs = u32(Shader::SwStage::Fragment);
    return (fs < modules.size() && modules[fs]) || runtime_infos[fs] == Shader::RuntimeInfo{};
}

class GraphicsPipeline : public Pipeline {
public:
    struct SerializationSupport {
        VertexInputs<vk::VertexInputAttributeDescription> vertex_attributes{};
        VertexInputs<vk::VertexInputBindingDescription> vertex_bindings{};
        VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT> divisors{};
        vk::PipelineMultisampleStateCreateInfo multisampling{};
        std::vector<u32> tcs{};
        std::vector<u32> tes{};
        std::vector<u32> fragment{};

        void Serialize(Serialization::Archive& ar) const;
        bool Deserialize(Serialization::Archive& ar);
    };

    GraphicsPipeline(const Instance& instance, Scheduler& scheduler, DescriptorHeap& desc_heap,
                     const Shader::Profile& profile, const GraphicsPipelineKey& key,
                     vk::PipelineCache pipeline_cache,
                     std::span<const Shader::Info*, MaxShaderStages> stages,
                     std::span<const Shader::RuntimeInfo, MaxShaderStages> runtime_infos,
                     std::optional<const Shader::Gcn::FetchShaderData> fetch_shader,
                     std::span<const vk::ShaderModule> modules, SerializationSupport& sdata,
                     bool preloading, bool deferred = false);
    ~GraphicsPipeline();

    /// bbport: `deferred` construction sets the stages, key and fetch shader and keeps the
    /// rest of its inputs; this builds the layouts and the VkPipeline from them (on a worker,
    /// PipelineCache) and returns the serialization data the build filled in.
    SerializationSupport& BuildDeferred();

    const std::optional<const Shader::Gcn::FetchShaderData>& GetFetchShader() const noexcept {
        return fetch_shader;
    }

    /// bbport (Apple): the ES/GS compute pipelines when this pipeline draws an emulated geometry
    /// stage (vk_gs_emulation.h), else null.
    const GsEmulationStages* GsEmulation() const noexcept {
        return gs_emulation.get();
    }
    void SetGsEmulation(std::unique_ptr<GsEmulationStages> stages);

    bool UsesVertexPulling() const noexcept {
        return profile.emulate_vertex_fetch;
    }

    const GraphicsPipelineKey& GetGraphicsKey() const {
        return key;
    }

    /// macOS depth-bounds emulation: the layout of descriptor set 1 (the stored depth the
    /// fragment shader tests), or null when the pipeline does not emulate the test.
    vk::DescriptorSetLayout DepthBoundsLayout() const noexcept {
        WaitBuilt();
        return *depth_bounds_layout;
    }
    /// Commits and binds set 1 with the stored-depth view (DepthBoundsLayout non-null).
    void BindDepthBoundsImage(vk::ImageView view, vk::ImageLayout layout) const;

    /// Gets the attributes and bindings for vertex inputs.
    template <typename Attribute, typename Binding>
    void GetVertexInputs(VertexInputs<Attribute>& attributes, VertexInputs<Binding>& bindings,
                         VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT>& divisors,
                         VertexInputs<AmdGpu::Buffer>& guest_buffers, u32 step_rate_0,
                         u32 step_rate_1) const;

#ifdef __APPLE__
    /// BB_PIPELINE_VERIFY: what each stage was built from (PipelineCache::VerifyReusedPipeline).
    std::array<Shader::RuntimeInfo, MaxShaderStages> built_runtime_infos{};
    std::array<vk::ShaderModule, MaxShaderStages> built_modules{};
    bool built_preloaded = false;
#endif

private:
    std::unique_ptr<GsEmulationStages> gs_emulation;
    void BuildDescSetLayout(bool preloading);
    void Build(vk::PipelineCache pipeline_cache,
               std::span<const Shader::RuntimeInfo, MaxShaderStages> runtime_infos,
               std::span<const vk::ShaderModule> modules, SerializationSupport& sdata,
               bool preloading);
    struct DeferredInputs {
        vk::PipelineCache pipeline_cache;
        std::vector<Shader::RuntimeInfo> runtime_infos; ///< MaxShaderStages entries
        std::vector<vk::ShaderModule> modules;
        SerializationSupport sdata;
    };
    std::unique_ptr<DeferredInputs> deferred_inputs;

private:
    GraphicsPipelineKey key;
    std::optional<const Shader::Gcn::FetchShaderData> fetch_shader{};
    vk::UniqueDescriptorSetLayout depth_bounds_layout;
};

struct ClipDistanceShaderKey {
    std::array<std::tuple<u8, u8>, 8> clip_locations;

    bool operator==(const ClipDistanceShaderKey& key) const noexcept {
        return std::memcmp(this, &key, sizeof(key)) == 0;
    }
};

} // namespace Vulkan

template <>
struct std::hash<Vulkan::GraphicsPipelineKey> {
    std::size_t operator()(const Vulkan::GraphicsPipelineKey& key) const noexcept {
        return XXH3_64bits(&key, sizeof(key));
    }
};

template <>
struct std::hash<Vulkan::ClipDistanceShaderKey> {
    std::size_t operator()(const Vulkan::ClipDistanceShaderKey& key) const noexcept {
        return XXH3_64bits(&key, sizeof(key));
    }
};
