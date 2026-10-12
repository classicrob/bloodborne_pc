// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "bbport_toggles.h"
#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "common/alignment.h"
#include "common/assert.h"
#include "common/types.h"
#include "core/devtools/widget/gpu_memory.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"

namespace Vulkan {

using VideoCore::MemoryType;

namespace {

// bbport: staging memory is populated (allocated and cleared) by the kernel on first CPU access,
// ~2 ms per 16 MiB block. Streaming bursts a few seconds apart re-created the blocks trimmed
// in between and copies into them ran at 0.3 GB/s (stutter). Blocks now live 30 s idle, the
// upload ring keeps BB_STAGING_KEEP_MB (512) and is populated at startup.
constexpr u64 RING_IDLE_FRAMES = 3000;
constexpr u64 LARGE_IDLE_FRAMES = 3000;
constexpr u64 PREWARM_BLOCKS = 8;
constexpr u64 LARGE_MIN_GRANULARITY = 64_KB;
constexpr u64 BLOCK_SIZE = 16_MB;

u64 RoundAllocationSize(u64 size) {
    const u64 step = std::max(std::bit_floor(size) / 4, LARGE_MIN_GRANULARITY);
    return Common::AlignUp(size, step);
}

#ifdef __APPLE__
/// StagingReleaseAtRequest (diagnostic A/B): the release before 6850bc0.
bool ReleaseAtRequest() {
    static const bool whole_run = [] {
        const char* env = std::getenv("BB_STAGING_RELEASE_AT_REQUEST");
        const bool on = env && env[0] == '1';
        if (on) {
            std::fprintf(stderr, "\n*** BB_STAGING_RELEASE_AT_REQUEST=1: DIAGNOSTIC MODE. Staging "
                                 "buffers are released at their request tick (before 6850bc0); "
                                 "one may be reused before its last use. ***\n\n");
        }
        return on;
    }();
    const bool on = whole_run || BbToggle::Disabled(BbToggle::StagingReleaseAtRequest);
    static bool logged = false;
    if (on != logged) {
        std::fprintf(stderr, "Staging release: %s\n",
                     on ? "at request tick (diagnostic, before 6850bc0)" : "after last use");
        logged = on;
    }
    return on;
}
#endif
} // Anonymous namespace

StagingBufferPool::StagingBufferPool(const Instance& instance_, Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_} {
    const char* env = std::getenv("BB_STAGING_KEEP_MB");
    keep_blocks = (env ? std::strtoull(env, nullptr, 10) : 512) * 1_MB / BLOCK_SIZE;
    Ring& ring = rings[u32(MemoryType::HostUncached)];
    for (u64 i = 0; i < PREWARM_BLOCKS; ++i) {
        auto& block = ring.blocks.emplace_back(Block{
            .buffer = std::make_unique<VideoCore::StreamBuffer>(
                instance, scheduler, MemoryType::HostUncached, BLOCK_SIZE),
            .last_used_frame = frame,
        });
        if (!block.buffer->mapped_data.empty()) {
            std::memset(block.buffer->mapped_data.data(), 0, block.buffer->mapped_data.size());
        }
    }
}

StagingBufferPool::~StagingBufferPool() = default;

StagingBufferRef StagingBufferPool::Request(u64 size, MemoryType type, u64 alignment,
                                            bool deferred, bool retire) {
    std::scoped_lock lk{mutex};
#ifdef __APPLE__
    if (retire && ReleaseAtRequest()) {
        // Released now, as before 6850bc0; Retire then finds it here and does nothing.
        const StagingBufferRef ref = deferred || size > BLOCK_SIZE
                                         ? RequestLarge(size, type, deferred, false)
                                         : RequestFromRing(rings[u32(type)], size, alignment, type);
        released_at_request.push_back({ref.buffer, ref.offset, 0});
        return ref;
    }
#endif
    Ring& ring = rings[u32(type)];
    if (deferred || size > BLOCK_SIZE) {
        return RequestLarge(size, type, deferred, retire);
    }
    const StagingBufferRef ref = RequestFromRing(ring, size, alignment, type);
    if (retire) {
        const auto* block = static_cast<const VideoCore::StreamBuffer*>(ref.buffer);
        ring_retires.push_back({ref.buffer, ref.offset, block->LastTick()});
    }
    return ref;
}

StagingBufferRef StagingBufferPool::RequestFromRing(Ring& ring, u64 size, u64 alignment,
                                                    MemoryType type) {
    Block* best{};
    u64 best_offset{};
    for (size_t i = 0; i < ring.blocks.size(); ++i) {
        const size_t index = (ring.current + i) % ring.blocks.size();
        Block* block = &ring.blocks[index];
        if (const auto offset = block->buffer->Reserve(size, alignment, false)) {
            ring.current = index;
            best = block;
            best_offset = *offset;
            break;
        }
    }

    if (!best) {
        best = &ring.blocks.emplace_back(Block{
            .buffer =
                std::make_unique<VideoCore::StreamBuffer>(instance, scheduler, type, BLOCK_SIZE),
            .last_used_frame = frame,
        });
        ring.current = ring.blocks.size() - 1;
        best_offset = *best->buffer->Reserve(size, alignment, false);
    }

    auto& buffer = *best->buffer;
    best->last_used_frame = frame;

    StagingBufferRef ref{
        .buffer = &buffer,
        .offset = best_offset,
        .size = size,
        .type = type,
    };
    if (!buffer.mapped_data.empty()) {
        ref.mapped = buffer.mapped_data.data() + best_offset;
    }
    return ref;
}

StagingBufferRef StagingBufferPool::RequestLarge(u64 size, MemoryType type, bool deferred,
                                                 bool retire) {
    auto& cache = large_caches[u32(type)];
    const u64 max_size = std::max(size + size / 4, RoundAllocationSize(size));

    LargeBuffer* best{};
    for (LargeBuffer& entry : cache) {
        const u64 entry_size = entry.buffer->SizeBytes();
        if (entry.held || entry_size < size || entry_size > max_size ||
            !scheduler.IsFree(entry.tick)) {
            continue;
        }
        if (!best || entry_size < best->buffer->SizeBytes()) {
            best = &entry;
        }
    }

    if (!best) {
        best = &cache.emplace_back(LargeBuffer{
            .buffer = std::make_unique<VideoCore::Buffer>(instance, 0, RoundAllocationSize(size),
                                                          type, "StagingBufferPool:Dedicated"),
            .tick = 0,
            .last_used_frame = frame,
            .held = false,
        });
    }

    best->tick = scheduler.CurrentTick();
    best->last_used_frame = frame;
    best->held = deferred || retire;
    best->retiring = retire && !deferred;

    VideoCore::Buffer& buffer = *best->buffer;
    StagingBufferRef ref{
        .buffer = &buffer,
        .offset = 0,
        .size = size,
        .type = type,
    };
    if (!buffer.mapped_data.empty()) {
        ref.mapped = buffer.mapped_data.data();
    }
    return ref;
}

void StagingBufferPool::FreeDeferred(const StagingBufferRef& ref) {
    std::scoped_lock lk{mutex};
    auto& cache = large_caches[u32(ref.type)];
    const auto it = std::ranges::find_if(
        cache, [&](const LargeBuffer& entry) { return entry.buffer.get() == ref.buffer; });
    ASSERT_MSG(it != cache.end() && it->held, "FreeDeferred on an allocation that is not held");
    it->held = false;
    it->tick = scheduler.CurrentTick();
    it->last_used_frame = frame;
}

void StagingBufferPool::Retire(const VideoCore::Buffer* buffer, u64 offset) {
    {
        std::scoped_lock lk{mutex};
        const auto released_it =
            std::ranges::find_if(released_at_request, [&](const RingRetire& entry) {
                return entry.buffer == buffer && entry.offset == offset;
            });
        if (released_it != released_at_request.end()) {
            released_at_request.erase(released_it); // StagingReleaseAtRequest: nothing to do
            return;
        }
        const auto ring_it = std::ranges::find_if(ring_retires, [&](const RingRetire& entry) {
            return entry.buffer == buffer && entry.offset == offset;
        });
        if (ring_it != ring_retires.end()) {
            const u64 tick = ring_it->tick;
            ring_retires.erase(ring_it);
            // The last use was recorded on this thread just before: it lands in CurrentTick().
            ASSERT_MSG(scheduler.CurrentTick() == tick,
                       "Staging ring range at {:#x}: its block watches submission {}, its last "
                       "use lands in submission {}; the range could be reused before that use",
                       offset, tick, scheduler.CurrentTick());
            return;
        }
        const bool large = std::ranges::any_of(large_caches, [&](const auto& cache) {
            return std::ranges::any_of(cache, [&](const LargeBuffer& entry) {
                return entry.buffer.get() == buffer && entry.retiring;
            });
        });
        ASSERT_MSG(large, "Staging allocation retired twice or not requested with retire");
    }
    // Runs where the commands before it are recorded. CurrentTick() there is the submission
    // they land in, or the next one while a submission is syncing: never an earlier one.
    // bbport (Apple, asynchronous submission): the owner's open value can be several
    // submissions ahead of the recording thread; RecordingTick() is the one recorded into.
    scheduler.Record([this, buffer](vk::CommandBuffer) {
#ifdef __APPLE__
        const u64 tick = scheduler.RecordingTick();
#else
        const u64 tick = scheduler.CurrentTick();
#endif
        std::scoped_lock lk{mutex};
        for (auto& cache : large_caches) {
            for (auto& entry : cache) {
                if (entry.buffer.get() != buffer || !entry.retiring) {
                    continue;
                }
                if (tick > entry.tick + 1 && ++late_retires <= 16) {
                    LOG_ERROR(Render_Vulkan,
                              "Staging buffer ({} MB): retirement tick {} is two or more "
                              "submissions after its request tick {}; released by its request "
                              "tick it could have been freed before its last use",
                              entry.buffer->SizeBytes() >> 20, tick, entry.tick);
                }
                entry.tick = std::max(entry.tick, tick);
                entry.held = false;
                entry.retiring = false;
                return;
            }
        }
        UNREACHABLE_MSG("Staging buffer retired twice or not held");
    });
}

void StagingBufferPool::TickFrame() {
    std::scoped_lock lk{mutex};
    ++frame;
    for (Ring& ring : rings) {
        TrimRing(ring);
    }
    for (auto& cache : large_caches) {
        TrimLarge(cache);
    }
    PublishStats();
}

void StagingBufferPool::TrimRing(Ring& ring) {
    auto& blocks = ring.blocks;
    for (size_t i = blocks.size(); i-- > 0;) {
        const Block& block = blocks[i];
        if (blocks.size() <= keep_blocks || frame - block.last_used_frame < RING_IDLE_FRAMES ||
            !scheduler.IsFree(block.buffer->LastTick())) {
            continue;
        }
        blocks.erase(blocks.begin() + i);
        if (i < ring.current) {
            --ring.current;
        } else if (i == ring.current) {
            ring.current = 0;
        }
    }
    if (ring.current >= blocks.size()) {
        ring.current = 0;
    }
}

void StagingBufferPool::TrimLarge(std::vector<LargeBuffer>& cache) {
    std::erase_if(cache, [this](const LargeBuffer& entry) {
        return !entry.held && frame - entry.last_used_frame >= LARGE_IDLE_FRAMES &&
               scheduler.IsFree(entry.tick);
    });
}

void StagingBufferPool::PublishStats() const {
    using namespace Core::Devtools::Widget;
    if (!GpuMemoryViewer::IsEnabled()) {
        return;
    }
    static constexpr std::array<const char*, NUM_TYPES> names = {"DeviceLocal", "HostUncached",
                                                                 "HostCached"};
    GpuMemoryGroup group{.name = "Staging"};
    for (size_t i = 0; i < NUM_TYPES; ++i) {
        u64 bytes = 0;
        for (const Block& block : rings[i].blocks) {
            bytes += block.buffer->SizeBytes();
        }
        for (const LargeBuffer& entry : large_caches[i]) {
            bytes += entry.buffer->SizeBytes();
        }
        if (bytes != 0) {
            group.rows.push_back({.name = names[i], .bytes = bytes});
        }
    }
    GpuMemoryViewer::Publish(std::move(group));
}

} // namespace Vulkan
