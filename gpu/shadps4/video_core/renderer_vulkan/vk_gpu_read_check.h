// SPDX-License-Identifier: GPL-2.0-or-later
// bbport (Apple): BB_GPU_READ_CHECK's checker (vk_rasterizer.cpp Rasterizer::GpuReadCheck, which
// documents what it proves). In a header so that Rasterizer can own it.
#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <csetjmp>
#include <cstring>
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
        std::chrono::steady_clock::time_point recorded; ///< when it was hashed first
        float record_to_check_ms = 0; ///< filled by the checker
        float retired_to_check_ms = 0; ///< from the checker seeing the tick retired
        u64 stage_hash = 0; ///< the program of the stage that binds it (vs/ps name the draw)
        /// A range of at most SmallBytes: its bytes at record, and at the check once it changed,
        /// so the report names the bytes that changed (a V# range can be larger than what the
        /// shader reads, and the guest may own the bytes past it).
        std::array<u8, 64> recorded_bytes{};
        std::array<u8, 64> checked_bytes{};
    };
    static constexpr u32 SmallBytes = 64;
    struct Batch {
        u64 tick;
        std::vector<Range> ranges;
    };
    /// Draws queued and not yet checked; more are dropped (counted) rather than growing without
    /// bound while the GPU is far behind.
    static constexpr std::size_t MaxQueued = 65536;
    /// Differences waiting for Report (the recording thread may stall); more are dropped, counted.
    static constexpr std::size_t MaxChanged = 65536;

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
        u64 checked_now, checked_bytes_now, unreadable_now, dropped_now, dropped_changed_now;
        {
            std::scoped_lock lock{mutex};
            found.swap(changed);
            checked_now = checked;
            checked_bytes_now = checked_bytes;
            unreadable_now = unreadable;
            dropped_now = dropped;
            dropped_changed_now = dropped_changed;
        }
        for (const auto& range : found) {
            // Current tracker state, not history: marked GPU-modified now (later GPU work wrote it,
            // most likely, ordered) or not.
            if (buffer_cache.IsRegionGpuModified(range.address, range.size)) {
                ++marked_gpu_modified;
                continue;
            }
            ++unmarked_changed;
            auto& entry = by_binding[{range.vs, range.ps, range.stage, range.binding, range.kind}];
            ++entry.count;
            entry.size = range.size;
            entry.address = range.address;
            const auto [first, last] = ChangedBytes(range);
            if (first <= last) {
                entry.first_changed = std::min(entry.first_changed, first);
                entry.last_changed = std::max(entry.last_changed, last);
            }
            if (unmarked_changed <= 64 || (unmarked_changed & 1023) == 0) {
                std::printf("GPU read check: changed between record and a check after GPU "
                            "retirement, not currently marked GPU-modified (#%llu): frame %llu "
                            "draw #%llu %s stage %u (program %#llx) binding %u vs %#llx ps %#llx "
                            "range %#llx+%u; "
                            "checked %.1f ms after record, %.1f ms after the checker saw the "
                            "tick retired\n",
                            (unsigned long long)unmarked_changed, (unsigned long long)range.frame,
                            (unsigned long long)range.sequence, KindName(range.kind),
                            u32(range.stage), (unsigned long long)range.stage_hash,
                            u32(range.binding), (unsigned long long)range.vs,
                            (unsigned long long)range.ps, (unsigned long long)range.address,
                            range.size, range.record_to_check_ms, range.retired_to_check_ms);
                PrintChangedWords(range);
            }
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - last_summary < std::chrono::seconds(5)) {
            return;
        }
        last_summary = now;
        std::printf("GPU read check: %llu ranges (%llu MB) checked; changed: %llu not currently "
                    "marked GPU-modified, %llu currently marked GPU-modified at report; %llu "
                    "unreadable at the check; dropped: %llu draws (queue full), %llu differences "
                    "(report backlog full)\n",
                    (unsigned long long)checked_now, (unsigned long long)(checked_bytes_now >> 20),
                    (unsigned long long)unmarked_changed, (unsigned long long)marked_gpu_modified,
                    (unsigned long long)unreadable_now, (unsigned long long)dropped_now,
                    (unsigned long long)dropped_changed_now);
        // The bindings that changed most since the last summary.
        std::vector<std::pair<BindingKey, BindingEntry>> top(by_binding.begin(), by_binding.end());
        std::ranges::sort(top, [](const auto& a, const auto& b) {
            return a.second.count > b.second.count;
        });
        for (std::size_t i = 0; i < std::min<std::size_t>(top.size(), 12); ++i) {
            const auto& [key, entry] = top[i];
            std::printf("  %llu x %s stage %u binding %u vs %#llx ps %#llx (last %#llx+%u)",
                        (unsigned long long)entry.count, KindName(key.kind), u32(key.stage),
                        u32(key.binding), (unsigned long long)key.vs, (unsigned long long)key.ps,
                        (unsigned long long)entry.address, entry.size);
            if (entry.first_changed <= entry.last_changed) {
                std::printf("; bytes +%u..+%u changed", entry.first_changed, entry.last_changed);
            }
            std::printf("\n");
        }
        by_binding.clear();
        std::fflush(stdout);
    }

private:
    /// The first and last byte offsets that differ, for a range small enough to have its bytes
    /// kept; first > last otherwise.
    static std::pair<u32, u32> ChangedBytes(const Range& range) {
        u32 first = ~0u, last = 0;
        if (range.size <= SmallBytes) {
            for (u32 at = 0; at < range.size; ++at) {
                if (range.recorded_bytes[at] != range.checked_bytes[at]) {
                    first = std::min(first, at);
                    last = at;
                }
            }
        }
        return {first, last};
    }

    /// Recording thread: the changed 32-bit words of a small range, record -> check.
    static void PrintChangedWords(const Range& range) {
        const auto [first, last] = ChangedBytes(range);
        if (first > last) {
            return;
        }
        std::printf("  bytes +%u..+%u of %u changed:", first, last, range.size);
        for (u32 at = first & ~3u; at <= last && at + 4 <= range.size; at += 4) {
            u32 before, after;
            std::memcpy(&before, range.recorded_bytes.data() + at, 4);
            std::memcpy(&after, range.checked_bytes.data() + at, 4);
            if (before != after) {
                std::printf(" +%u %#010x->%#010x", at, before, after);
            }
        }
        std::printf("\n");
    }

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
            const auto retired_seen = std::chrono::steady_clock::now();
            std::vector<Range> different;
            u64 bytes = 0, failed = 0;
            for (const auto& range : batch.ranges) {
                bytes += range.size;
                // The guest may have unmapped the range since: a fault skips it.
                auto* const previous = runtime_fault_recover;
                sigjmp_buf recover;
                if (sigsetjmp(recover, 0)) {
                    runtime_fault_recover = previous;
                    ++failed;
                    continue;
                }
                runtime_fault_recover = &recover;
                const u64 hash =
                    XXH3_64bits(reinterpret_cast<const void*>(range.address), range.size);
                std::array<u8, SmallBytes> now{};
                if (hash != range.hash && range.size <= SmallBytes) {
                    std::memcpy(now.data(), reinterpret_cast<const void*>(range.address),
                                range.size);
                }
                runtime_fault_recover = previous;
                if (hash != range.hash) {
                    const auto checked_at = std::chrono::steady_clock::now();
                    Range found = range;
                    found.checked_bytes = now;
                    found.record_to_check_ms =
                        std::chrono::duration<float, std::milli>(checked_at - range.recorded)
                            .count();
                    found.retired_to_check_ms =
                        std::chrono::duration<float, std::milli>(checked_at - retired_seen).count();
                    different.push_back(found);
                }
            }
            std::scoped_lock lock{mutex};
            checked += batch.ranges.size();
            checked_bytes += bytes;
            unreadable += failed;
            for (const auto& range : different) {
                if (changed.size() >= MaxChanged) {
                    ++dropped_changed;
                    continue;
                }
                changed.push_back(range);
            }
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
        u32 first_changed = ~0u, last_changed = 0; ///< over its small ranges since the summary
    };

    const Instance& instance;
    Scheduler& scheduler;
    // Checker thread and recording thread, under mutex.
    std::mutex mutex;
    std::condition_variable_any cv;
    std::deque<Batch> queue;
    std::vector<Range> changed;
    u64 checked = 0, checked_bytes = 0, unreadable = 0, dropped = 0, dropped_changed = 0;
    // Recording thread only.
    u64 unmarked_changed = 0, marked_gpu_modified = 0;
    std::map<BindingKey, BindingEntry> by_binding;
    std::chrono::steady_clock::time_point last_summary = std::chrono::steady_clock::now();
    std::jthread thread; ///< last: stopped and joined before the members it uses go
};


} // namespace Vulkan
