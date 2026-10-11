// SPDX-License-Identifier: GPL-2.0-or-later
// bbport (Apple): BB_GPU_READ_CHECK's checker (vk_rasterizer.cpp Rasterizer::GpuReadCheck, which
// documents what it proves). In a header so that Rasterizer can own it.
#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <csetjmp>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <stop_token>
#include <thread>
#include <vector>
#include <xxhash.h>

#include "bbport_toggles.h"
#include "common/assert.h"
#include "common/thread.h"
#include "common/types.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

class GpuReadChecker {
public:
    struct Range {
        VAddr address;
        u32 size;
        u64 hash;
        u64 vs, ps;
        u64 frame, sequence;
        char kind; ///< 'b' read-only buffer, 'v' vertex buffer, 'i' index range
        u8 stage;
        u16 binding;
    };
    struct Batch {
        u64 tick;
        std::vector<Range> ranges;
    };
    /// Draws queued and not yet checked; more are dropped (counted) rather than growing without
    /// bound while the GPU is far behind.
    static constexpr std::size_t MaxQueued = 65536;

    GpuReadChecker(const Instance& instance_, Scheduler& scheduler_)
        : instance{instance_}, scheduler{scheduler_},
          thread{[this](std::stop_token stop) { Run(stop); }} {}

    /// Recording thread: a draw's ranges, hashed just after it was recorded.
    void Submit(Batch&& batch) {
        {
            std::scoped_lock lock{mutex};
            if (queue.size() >= MaxQueued) {
                ++dropped;
                return;
            }
            queue.push_back(std::move(batch));
        }
        cv.notify_one();
    }

    /// Recording thread: classifies and prints the differences the checker found (the buffer
    /// tracker belongs to this thread), and the summary.
    void Report(VideoCore::BufferCache& buffer_cache) {
        std::vector<Range> found;
        u64 checked_now, checked_bytes_now, unreadable_now, dropped_now;
        {
            std::scoped_lock lock{mutex};
            found.swap(changed);
            checked_now = checked;
            checked_bytes_now = checked_bytes;
            unreadable_now = unreadable;
            dropped_now = dropped;
        }
        for (const auto& range : found) {
            if (buffer_cache.IsRegionGpuModified(range.address, range.size)) {
                ++gpu_written; // later GPU work wrote it, in stream order
                continue;
            }
            ++guest_changed;
            auto& entry = by_binding[{range.vs, range.ps, range.stage, range.binding, range.kind}];
            ++entry.count;
            entry.size = range.size;
            entry.address = range.address;
            if (guest_changed <= 64 || (guest_changed & 1023) == 0) {
                std::printf("GPU read check: changed between record and the check after GPU "
                            "retirement (#%llu): frame %llu draw #%llu %s stage %u binding %u "
                            "vs %#llx ps %#llx range %#llx+%u\n",
                            (unsigned long long)guest_changed, (unsigned long long)range.frame,
                            (unsigned long long)range.sequence, KindName(range.kind),
                            u32(range.stage), u32(range.binding), (unsigned long long)range.vs,
                            (unsigned long long)range.ps, (unsigned long long)range.address,
                            range.size);
            }
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - last_summary < std::chrono::seconds(5)) {
            return;
        }
        last_summary = now;
        std::printf("GPU read check: %llu ranges (%llu MB) checked, %llu changed (not GPU-written), "
                    "%llu GPU-written since, %llu unreadable at the check, %llu draws dropped "
                    "(queue full)\n",
                    (unsigned long long)checked_now, (unsigned long long)(checked_bytes_now >> 20),
                    (unsigned long long)guest_changed, (unsigned long long)gpu_written,
                    (unsigned long long)unreadable_now, (unsigned long long)dropped_now);
        // The bindings that changed most since the last summary.
        std::vector<std::pair<BindingKey, BindingEntry>> top(by_binding.begin(), by_binding.end());
        std::ranges::sort(top, [](const auto& a, const auto& b) {
            return a.second.count > b.second.count;
        });
        for (std::size_t i = 0; i < std::min<std::size_t>(top.size(), 12); ++i) {
            const auto& [key, entry] = top[i];
            std::printf("  %llu x %s stage %u binding %u vs %#llx ps %#llx (last %#llx+%u)\n",
                        (unsigned long long)entry.count, KindName(key.kind), u32(key.stage),
                        u32(key.binding), (unsigned long long)key.vs, (unsigned long long)key.ps,
                        (unsigned long long)entry.address, entry.size);
        }
        by_binding.clear();
        std::fflush(stdout);
    }

private:
    static const char* KindName(char kind) {
        return kind == 'i' ? "index range" : kind == 'v' ? "vertex buffer" : "read-only buffer";
    }

    bool WaitTick(u64 tick, const std::stop_token& stop) {
        while (!scheduler.IsFree(tick)) {
            if (stop.stop_requested()) {
                return false;
            }
            const vk::Semaphore semaphore = scheduler.GetWorkSemaphore()->Handle();
            const vk::SemaphoreWaitInfo wait_info = {
                .semaphoreCount = 1, .pSemaphores = &semaphore, .pValues = &tick};
            const vk::Result result = instance.GetDevice().waitSemaphores(&wait_info, 10'000'000);
            ASSERT_MSG(result == vk::Result::eSuccess || result == vk::Result::eTimeout,
                       "BB_GPU_READ_CHECK: the wait for timeline value {} failed: {}", tick,
                       vk::to_string(result));
        }
        return true;
    }

    void Run(std::stop_token stop) {
        Common::SetCurrentThreadName("bb:GpuReadCheck");
        while (!stop.stop_requested()) {
            Batch batch;
            {
                std::unique_lock lock{mutex};
                cv.wait(lock, stop, [this] { return !queue.empty(); });
                if (stop.stop_requested()) {
                    return;
                }
                batch = std::move(queue.front());
                queue.pop_front();
            }
            if (!WaitTick(batch.tick, stop)) {
                return;
            }
            std::vector<Range> different;
            u64 bytes = 0, failed = 0;
            for (const auto& range : batch.ranges) {
                bytes += range.size;
                // The guest may have unmapped the range since: a fault skips it.
                sigjmp_buf recover;
                if (sigsetjmp(recover, 0)) {
                    runtime_fault_recover = nullptr;
                    ++failed;
                    continue;
                }
                runtime_fault_recover = &recover;
                const u64 hash =
                    XXH3_64bits(reinterpret_cast<const void*>(range.address), range.size);
                runtime_fault_recover = nullptr;
                if (hash != range.hash) {
                    different.push_back(range);
                }
            }
            std::scoped_lock lock{mutex};
            checked += batch.ranges.size();
            checked_bytes += bytes;
            unreadable += failed;
            changed.insert(changed.end(), different.begin(), different.end());
        }
    }

    struct BindingKey {
        u64 vs, ps;
        u8 stage;
        u16 binding;
        char kind;
        auto operator<=>(const BindingKey&) const = default;
    };
    struct BindingEntry {
        u64 count = 0;
        VAddr address = 0;
        u32 size = 0;
    };

    const Instance& instance;
    Scheduler& scheduler;
    // Checker thread and recording thread, under mutex.
    std::mutex mutex;
    std::condition_variable_any cv;
    std::deque<Batch> queue;
    std::vector<Range> changed;
    u64 checked = 0, checked_bytes = 0, unreadable = 0, dropped = 0;
    // Recording thread only.
    u64 guest_changed = 0, gpu_written = 0;
    std::map<BindingKey, BindingEntry> by_binding;
    std::chrono::steady_clock::time_point last_summary = std::chrono::steady_clock::now();
    std::jthread thread; ///< last: stopped and joined before the members it uses go
};


} // namespace Vulkan
