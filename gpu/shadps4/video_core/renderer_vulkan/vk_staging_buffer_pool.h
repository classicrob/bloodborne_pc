// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <mutex>

#include <array>
#include <memory>
#include <vector>

#include "common/types.h"
#include "video_core/buffer_cache/buffer.h"

namespace Vulkan {

class Instance;
class Scheduler;

struct StagingBufferRef {
    VideoCore::Buffer* buffer{};
    u64 offset{};
    u64 size{};
    u8* mapped{};
    VideoCore::MemoryType type{};

    void Flush() const noexcept {
        buffer->Flush(offset, size);
    }

    void Invalidate() const noexcept {
        buffer->Invalidate(offset, size);
    }
};

class StagingBufferPool {
public:
    explicit StagingBufferPool(const Instance& instance, Scheduler& scheduler);
    ~StagingBufferPool();

    StagingBufferPool(const StagingBufferPool&) = delete;
    StagingBufferPool& operator=(const StagingBufferPool&) = delete;

    /// Requests size bytes of memory of the given type. `retire`: the caller records the uses
    /// later and calls Retire after the last one (see there).
    [[nodiscard]] StagingBufferRef Request(u64 size, VideoCore::MemoryType type, u64 alignment = 0,
                                           bool deferred = false, bool retire = false);

    /// Releases a deferred allocation. It becomes reusable once the current tick completes.
    void FreeDeferred(const StagingBufferRef& ref);

    /// bbport: after the last use of a `retire` allocation was recorded, on the thread that
    /// recorded it. A large allocation is released by a recorded marker with the tick of the
    /// submission that marker (and so that use) lands in, which can be later than the tick at
    /// Request: another submission may come between. A ring range keeps the tick its block
    /// watched at Request (the watches are in order and cannot move), so a submission between
    /// its Request and here is a hard error.
    void Retire(const VideoCore::Buffer* buffer, u64 offset);

    /// What the pool holds, per memory type (DeviceLocal, HostUncached, HostCached): ring
    /// blocks and dedicated (large) buffers, their bytes, and the large ones held (requested
    /// with deferred or retire and not released yet). Any thread (takes the pool mutex); the
    /// perf replay prints it per iteration.
    struct TypeTotals {
        u64 ring_blocks{}, ring_bytes{}, large_buffers{}, large_bytes{}, large_held{};
    };
    [[nodiscard]] std::array<TypeTotals, 3> Totals();

    /// Releases idle memory.
    void TickFrame();

private:
    struct Block {
        std::unique_ptr<VideoCore::StreamBuffer> buffer;
        u64 last_used_frame;
    };

    struct Ring {
        std::vector<Block> blocks;
        size_t current = 0;
    };

    struct LargeBuffer {
        std::unique_ptr<VideoCore::Buffer> buffer;
        u64 tick;
        u64 last_used_frame;
        bool held;
        bool retiring = false; ///< held until Retire's marker is recorded
    };

    static constexpr size_t NUM_TYPES = 3;

    StagingBufferRef RequestFromRing(Ring& ring, u64 size, u64 alignment,
                                     VideoCore::MemoryType type);
    StagingBufferRef RequestLarge(u64 size, VideoCore::MemoryType type, bool deferred,
                                  bool retire);

    void TrimRing(Ring& ring);
    void TrimLarge(std::vector<LargeBuffer>& cache);

    void PublishStats() const;

    const Instance& instance;
    Scheduler& scheduler;

    std::array<Ring, NUM_TYPES> rings;
    u64 keep_blocks = 0; ///< bbport: ring blocks never trimmed
    std::array<std::vector<LargeBuffer>, NUM_TYPES> large_caches;
    u64 frame = 0;
    /// bbport: ring ranges requested with `retire`, and the tick their block watched then.
    struct RingRetire {
        const VideoCore::Buffer* buffer;
        u64 offset;
        u64 tick;
    };
    std::vector<RingRetire> ring_retires;
    /// bbport: `retire` allocations released at their request (StagingReleaseAtRequest A/B).
    std::vector<RingRetire> released_at_request;
    /// bbport: FreeDeferred runs on the priority runner, Retire's marker on the recording
    /// thread, the rest on the thread recording commands.
    std::mutex mutex;
    u64 late_retires = 0; ///< uses recorded two or more submissions after their Request tick
};

} // namespace Vulkan
