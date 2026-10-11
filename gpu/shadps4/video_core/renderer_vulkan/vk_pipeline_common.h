// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include "shader_recompiler/profile.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/renderer_vulkan/vk_common.h"

#include <boost/container/small_vector.hpp>

namespace Shader {
struct Info;
struct PushData;
} // namespace Shader

namespace Vulkan {

static constexpr auto AllGraphicsStageBits =
    vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eTessellationControl |
    vk::ShaderStageFlagBits::eTessellationEvaluation | vk::ShaderStageFlagBits::eGeometry |
    vk::ShaderStageFlagBits::eFragment;

class Instance;
class Scheduler;
class DescriptorHeap;

class Pipeline {
public:
    Pipeline(const Instance& instance, Scheduler& scheduler, DescriptorHeap& desc_heap,
             const Shader::Profile& profile, vk::PipelineCache pipeline_cache,
             bool is_compute = false);
    virtual ~Pipeline();

    vk::Pipeline Handle() const noexcept {
        WaitBuilt();
        return *pipeline;
    }

    vk::PipelineLayout GetLayout() const noexcept {
        WaitBuilt();
        return *pipeline_layout;
    }

    /// bbport: a pipeline can be built on a worker thread after it is handed out (PipelineCache
    /// on Apple). Its stages, key and fetch shader are set at once; everything the build creates
    /// (layouts, the VkPipeline) is read through accessors that wait for it here.
    void WaitBuilt() const noexcept {
        if (!built.load(std::memory_order_acquire)) [[unlikely]] {
            WaitBuiltSlow();
        }
    }
    bool IsBuilt() const noexcept {
        return built.load(std::memory_order_acquire);
    }

    auto GetStages() const {
        static_assert(static_cast<u32>(Shader::SwStage::Compute) == Shader::MaxStageTypes - 1);
        if (is_compute) {
            return std::span{stages.cend() - 1, stages.cend()};
        } else {
            return std::span{stages.cbegin(), stages.cend() - 1};
        }
    }

    const Shader::Info& GetStage(Shader::SwStage stage) const noexcept {
        return *stages[u32(stage)];
    }

    bool IsCompute() const {
        return is_compute;
    }

    using DescriptorWrites = std::vector<vk::WriteDescriptorSet>;
    void BindResources(DescriptorWrites& set_writes, const Shader::PushData& push_data) const;

protected:
    [[nodiscard]] std::string GetDebugString() const;
    void SetBuilt();
    void WaitBuiltSlow() const noexcept;

    const Instance& instance;
    Scheduler& scheduler;
    DescriptorHeap& desc_heap;
    const Shader::Profile& profile;
    vk::UniquePipeline pipeline;
    vk::UniquePipelineLayout pipeline_layout;
    vk::UniqueDescriptorSetLayout desc_layout;
    std::array<const Shader::Info*, Shader::MaxStageTypes> stages{};
    bool uses_push_descriptors{};
    bool is_compute;
    std::atomic<bool> built{true};
    mutable std::mutex build_mutex;
    mutable std::condition_variable build_cv;
};

/// bbport: time threads spent waiting for pipelines built on workers (BB_FRAME_STATS).
extern std::atomic<u64> g_bb_pipeline_wait_ns;

} // namespace Vulkan
