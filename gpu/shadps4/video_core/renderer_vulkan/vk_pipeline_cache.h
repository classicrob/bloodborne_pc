// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <deque>
#include <functional>
#include <unordered_map>
#include <shared_mutex>
#include <variant>
#include <boost/container/static_vector.hpp>
#include <tsl/robin_map.h>
#include "shader_recompiler/profile.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/specialization.h"
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_resource_pool.h"

template <>
struct std::hash<vk::ShaderModule> {
    std::size_t operator()(const vk::ShaderModule& module) const noexcept {
        return std::hash<size_t>{}(reinterpret_cast<size_t>((VkShaderModule)module));
    }
};

namespace AmdGpu {
class Liverpool;
}

namespace Serialization {
struct Archive;
}

namespace Shader {
struct Info;
}

namespace Vulkan {

class Instance;
class Scheduler;
class ShaderCache;

struct Program {
    struct Module {
        vk::ShaderModule module;
        Shader::StageSpecialization spec;
        // Pipeline resource lists must match this module, and remain stable across vector growth.
        std::unique_ptr<Shader::Info> info;
        // Workers refresh private copies; the GPU thread alone updates live user data.
        std::unique_ptr<const Shader::Info> info_template;
    };
    static constexpr size_t MaxPermutations = 8;
    using ModuleList = boost::container::small_vector<Module, MaxPermutations>;

    ModuleList modules{};
    size_t last_used = 0; ///< bbport: permutation of the previous lookup, compared first

    Program() = default;

    void AddPermut(vk::ShaderModule module, Shader::StageSpecialization&& spec,
                   Shader::Info&& metadata) {
        InsertPermut(module, std::move(spec), std::move(metadata), modules.size());
    }

    void InsertPermut(vk::ShaderModule module, Shader::StageSpecialization&& spec,
                      Shader::Info&& metadata, size_t perm_idx) {
        modules.resize(std::max(modules.size(), perm_idx + 1));
        if (modules[perm_idx].info) {
            ASSERT_MSG(modules[perm_idx].module == module && modules[perm_idx].spec == spec,
                       "Shader permutation metadata conflicts with an existing cache slot");
            return;
        }
        auto info = std::make_unique<Shader::Info>(std::move(metadata));
        // bbport: image descriptor counts as compiled into this permutation (its specialization
        // records every image's, null ones included). Pipeline layouts, binding offsets and
        // descriptor writes use these, also for pipelines rebuilt from the stage cache without
        // live user data.
        ASSERT_MSG(spec.images.size() == info->images.size(),
                   "Permutation of {:#x} specializes {} images, its info has {}", info->pgm_hash,
                   spec.images.size(), info->images.size());
        for (size_t i = 0; i < info->images.size(); ++i) {
            ASSERT_MSG(spec.images[i].num_bindings,
                       "Permutation of {:#x}: image {} specializes no binding count",
                       info->pgm_hash, i);
            info->images[i].permutation_bindings = spec.images[i].num_bindings;
        }
        auto snapshot = std::make_unique<const Shader::Info>(*info);
        spec.info = info.get();
        modules[perm_idx] = {module, std::move(spec), std::move(info), std::move(snapshot)};
    }
};

struct DrawIndirectParams {
    u16 vertex_sgpr_offset;
    u32 instance_sgpr_offset;
};

} // namespace Vulkan

namespace AmdGpu {
union Regs;
}

namespace Vulkan {

/// bbport: state of one graphics/compute pipeline selection. The GPU thread owns one
/// (PipelineCache::sel); draw-preparation workers use their own with their register copies.
struct PipelineSelection {
    const AmdGpu::Regs* regs{};
    std::array<Shader::RuntimeInfo, MaxShaderStages> runtime_infos{};
    std::array<const Shader::Info*, MaxShaderStages> infos{};
    std::array<vk::ShaderModule, MaxShaderStages> modules{};
    std::optional<Shader::Gcn::FetchShaderData> fetch_shader{};
    GraphicsPipelineKey graphics_key{};
    bool motion = false;
    /// bbport (Apple): ES and GS of an emulated geometry stage (vk_gs_emulation.h).
    struct GsEmulationProgram {
        const Shader::Info* info{};
        vk::ShaderModule module{};
        size_t hash{};
    } gs_es{}, gs_gs{};
    DrawIndirectParams draw_indirect_params{};
    struct PrepWorker* worker{}; ///< set: read-only selection for a draw-preparation worker
};

/// bbport: a draw-preparation worker's own program state (see vk_draw_prep.h).
struct PrepWorker {
    struct Stage {
        const Program* program;
        const Shader::Info* info;
        u64 hash;
        Shader::HwStage hw_stage;
        VAddr pgm_base;
        const std::vector<u32>* flat;
    };
    // Node-based: stages keep pointers to these Infos while later stages are inserted.
    std::unordered_map<const Shader::Info*, Shader::Info> infos;
    boost::container::static_vector<Stage, MaxShaderStages> stages;
    bool failed = false;
};

struct PreparedDraw;

class PipelineCache {
public:
    explicit PipelineCache(const Instance& instance, Scheduler& scheduler,
                           AmdGpu::Liverpool* liverpool, u32 sparse_page_shift);
    ~PipelineCache();

    void WarmUp();
    void Sync();

    /// bbport: pipelines WarmUp read from the store; their creation (the costly part: on Apple
    /// MoltenVK's conversion and the Metal compile) runs afterwards on worker threads.
    struct PreloadBatch {
        std::vector<std::function<void()>> creations;
        std::deque<std::pair<GraphicsPipelineKey, std::unique_ptr<GraphicsPipeline>>> graphics;
        std::deque<std::pair<ComputePipelineKey, std::unique_ptr<ComputePipeline>>> compute;
    };
    bool LoadComputePipeline(Serialization::Archive& ar, PreloadBatch& batch);
    bool LoadGraphicsPipeline(Serialization::Archive& ar, PreloadBatch& batch);
    bool LoadPipelineStage(Serialization::Archive& ar, size_t stage);
    /// bbport: clears the selection a pipeline record is loaded into.
    void ResetPreloadSelection();

    const GraphicsPipeline* GetGraphicsPipeline(const DrawIndirectParams params = {},
                                                const PreparedDraw* prepared = nullptr);

    /// bbport: worker side of draw preparation: selects the pipeline key for `sel.regs` without
    /// creating anything. False when a program or permutation does not exist yet.
    bool PrepareGraphicsPipeline(PipelineSelection& sel);

    /// bbport: GPU-thread side: the pipeline for a prepared draw after checking that registers
    /// and flattened user data match; null to take the regular path.
    const GraphicsPipeline* TryPreparedPipeline(const PreparedDraw& prepared);

    /// bbport: the prepared draw the last GetGraphicsPipeline used, or null (regular path).
    [[nodiscard]] const PreparedDraw* UsedPrepared() const noexcept {
        return used_prepared;
    }

    const ComputePipeline* GetComputePipeline();

    using Result = std::tuple<const Shader::Info*, vk::ShaderModule,
                              std::optional<Shader::Gcn::FetchShaderData>, u64>;
    Result GetProgram(PipelineSelection& sel, Shader::HwStage stage, Shader::SwStage l_stage,
                      const Shader::ShaderParams& params, Shader::Backend::Bindings& binding);

    std::optional<vk::ShaderModule> ReplaceShader(vk::ShaderModule module,
                                                  std::span<const u32> spv_code);

    static std::string GetShaderName(Shader::HwStage stage, u64 hash,
                                     std::optional<size_t> perm = {});

    auto& GetProfile() const {
        return profile;
    }

private:
    bool RefreshGraphicsKey(PipelineSelection& sel);
    bool RefreshGraphicsStages(PipelineSelection& sel);
    bool RefreshComputeKey();

    void DumpShader(std::span<const u32> code, u64 hash, Shader::HwStage stage, size_t perm_idx,
                    std::string_view ext);
    std::optional<std::vector<u32>> GetShaderPatch(u64 hash, Shader::HwStage stage, size_t perm_idx,
                                                   std::string_view ext);
    vk::ShaderModule CompileModule(Shader::Info& info, Shader::RuntimeInfo& runtime_info,
                                   const std::span<const u32>& code, size_t perm_idx,
                                   Shader::Backend::Bindings& binding);
    const Shader::RuntimeInfo& BuildRuntimeInfo(PipelineSelection& sel, Shader::HwStage stage,
                                                Shader::SwStage l_stage);

    [[nodiscard]] bool IsPipelineCacheDirty() const {
        return num_new_pipelines > 0;
    }

private:
    const Instance& instance;
    Scheduler& scheduler;
    AmdGpu::Liverpool* liverpool;
    DescriptorHeap desc_heap;
    vk::UniquePipelineCache pipeline_cache;
    vk::UniquePipelineLayout pipeline_layout;
    Shader::Profile profile{};
    Shader::Pools pools;
    tsl::robin_map<size_t, std::unique_ptr<Program>> program_cache;
    /// bbport: exclusive for program/permutation insertions, shared for worker lookups.
    std::shared_mutex programs_mutex;
    u64 prepared_hits = 0, prepared_misses = 0;
    const PreparedDraw* used_prepared = nullptr;
    tsl::robin_map<ComputePipelineKey, std::unique_ptr<ComputePipeline>> compute_pipelines;
    /// bbport (Apple): ES/GS compute pipelines of emulated geometry stages, keyed by stage
    /// permutation and shared by the graphics pipelines using them (vk_gs_emulation.h).
    tsl::robin_map<size_t, std::unique_ptr<ComputePipeline>> gs_compute_pipelines;
    tsl::robin_map<GraphicsPipelineKey, std::unique_ptr<GraphicsPipeline>> graphics_pipelines;
    PipelineSelection sel{}; ///< GPU thread selection state
    ComputePipelineKey compute_key{};
    u32 num_new_pipelines{}; // new pipelines added to the cache since the game start

    // Only if Config::collectShadersForDebug()
    tsl::robin_map<vk::ShaderModule,
                   std::vector<std::variant<GraphicsPipelineKey, ComputePipelineKey>>>
        module_related_pipelines;
};

} // namespace Vulkan
