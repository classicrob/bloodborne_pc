// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>

#include <deque>
#include <vector>
#include <boost/container/static_vector.hpp>
#include <tsl/robin_map.h>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

class Instance;
class Semaphore;

/**
 * Handles a pool of resources protected by fences. Manages resource overflow allocating more
 * resources.
 */
class ResourcePool {
public:
    explicit ResourcePool() = default;
    explicit ResourcePool(Semaphore* work_semaphore, std::size_t grow_step);
    virtual ~ResourcePool() = default;

    ResourcePool& operator=(ResourcePool&&) noexcept = default;
    ResourcePool(ResourcePool&&) noexcept = default;

    ResourcePool& operator=(const ResourcePool&) = default;
    ResourcePool(const ResourcePool&) = default;

public:
    /// Resources held (the pool grows by grow_step, up to max_resources when set); readable
    /// from any thread.
    [[nodiscard]] std::size_t Size() const noexcept {
        return published.size.load(std::memory_order_acquire);
    }
    /// Commits that waited for the GPU at max_resources (any thread).
    [[nodiscard]] std::uint64_t WaitsAtCap() const noexcept {
        return published.waits_at_cap.load(std::memory_order_acquire);
    }

protected:
    std::size_t CommitResource();
    /// bbport: stamps the resource with `tick` (the submission that will use it) instead of
    /// the current tick (asynchronous submission commits on the recording thread).
    std::size_t CommitResource(u64 tick);

    /// Called when a chunk of resources have to be allocated.
    virtual void Allocate(std::size_t begin, std::size_t end) = 0;

private:
    /// Manages pool overflow allocating new resources.
    std::size_t ManageOverflow();

protected:
    Semaphore* work_semaphore{};
    std::size_t grow_step = 0;     ///< Number of new resources created after an overflow
    std::size_t hint_iterator = 0; ///< Hint to where the next free resources is likely to be found
    std::vector<u64> ticks;        ///< Ticks for each resource
    std::size_t max_resources = 0; ///< bbport: 0 unbounded; else waits for the GPU at the cap
    /// Read from other threads (tests): ticks.size() published at each growth, and the commits
    /// that waited at the cap. Copyable so the pool keeps its defaulted copy and move.
    struct Published {
        std::atomic<std::size_t> size{0};
        std::atomic<std::uint64_t> waits_at_cap{0};
        Published() = default;
        Published(const Published& other)
            : size{other.size.load()}, waits_at_cap{other.waits_at_cap.load()} {}
        Published& operator=(const Published& other) {
            size = other.size.load();
            waits_at_cap = other.waits_at_cap.load();
            return *this;
        }
    } published;
};

class CommandPool final : public ResourcePool {
public:
    explicit CommandPool(const Instance& instance, Semaphore* work_semaphore);
    ~CommandPool() override;

    void Allocate(std::size_t begin, std::size_t end) override;

    vk::CommandBuffer Commit();
    /// bbport (Apple): command buffers in flight at most; a commit beyond waits for the GPU.
    static constexpr std::size_t MaxBuffers = 256;
    /// bbport: for the submission with timeline value `tick`.
    vk::CommandBuffer Commit(u64 tick);

private:
    const Instance& instance;
    vk::UniqueCommandPool cmd_pool;
    std::vector<vk::CommandBuffer> cmd_buffers;
};

class DescriptorHeap final {
    static constexpr u32 DescriptorSetBatch = 32;

public:
    explicit DescriptorHeap(const Instance& instance, Semaphore* work_semaphore,
                            std::span<const vk::DescriptorPoolSize> pool_sizes,
                            u32 descriptor_heap_count = 1024);
    ~DescriptorHeap();

    vk::DescriptorSet Commit(vk::DescriptorSetLayout set_layout);

private:
    void CreateDescriptorPool();

private:
    vk::Device device;
    Semaphore* work_semaphore;
    u32 descriptor_heap_count;
    std::span<const vk::DescriptorPoolSize> pool_sizes;
    vk::DescriptorPool curr_pool;
    std::deque<std::pair<vk::DescriptorPool, u64>> pending_pools;
    using DescSetBatch = boost::container::static_vector<vk::DescriptorSet, DescriptorSetBatch>;
    tsl::robin_map<u64, DescSetBatch> descriptor_sets;
};

} // namespace Vulkan
