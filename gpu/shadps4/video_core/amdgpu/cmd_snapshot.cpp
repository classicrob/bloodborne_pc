// SPDX-License-Identifier: GPL-2.0-or-later
// bbport (macOS): submit-time copies of graphics command memory (cmd_snapshot.h).

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <csetjmp>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>
#include "bbport_toggles.h"
#include "common/assert.h"
#include "common/logging/log.h"
#include "video_core/amdgpu/cmd_snapshot.h"
#include "video_core/amdgpu/pm4_cmds.h"

namespace AmdGpu {

namespace {

/// A stream can name indirect buffers inside regions it skips (COND_EXEC), so an IB target may be
/// unmapped garbage. These limits only stop such a stream from copying without end.
constexpr size_t MaxRegions = 4096;
constexpr size_t MaxWords = 64_MB / sizeof(u32);

/// Copies guest words through the speculative-read recovery point; false if the memory faults.
bool CopyGuest(u32* dst, const u32* src, u32 count) {
    sigjmp_buf recover;
    if (sigsetjmp(recover, 0)) {
        runtime_fault_recover = nullptr;
        return false;
    }
    runtime_fault_recover = &recover;
    std::memcpy(dst, src, size_t(count) * sizeof(u32));
    runtime_fault_recover = nullptr;
    return true;
}

struct StreamWrite {
    VAddr begin, end;
    u64 seq;
};
std::mutex stream_writes_mutex;
std::condition_variable stream_writes_landed;
std::deque<StreamWrite> stream_writes; ///< added, not landed yet; oldest first
u64 stream_writes_added = 0;
u32 stream_write_waiters = 0;
/// How recent writes landed, by sequence number, for a waiter to check the ones it waited for.
struct LandedWrite {
    u64 seq = ~u64{0};
    bool through_gpu = false;
};
std::array<LandedWrite, 4096> stream_writes_history;

} // namespace

void StreamWrites::Add(VAddr address, u64 size) {
    std::scoped_lock lk{stream_writes_mutex};
    stream_writes.push_back({address, address + size, stream_writes_added++});
}

void StreamWrites::Landed(bool through_gpu) {
    std::unique_lock lk{stream_writes_mutex};
    ASSERT_MSG(!stream_writes.empty(), "macOS: a stream write landed that was never added");
    const u64 seq = stream_writes.front().seq;
    stream_writes_history[seq % stream_writes_history.size()] = {seq, through_gpu};
    stream_writes.pop_front();
    if (stream_write_waiters) {
        lk.unlock();
        stream_writes_landed.notify_all();
    }
}

u32 StreamWrites::WaitLanded(VAddr address, u64 size) {
    std::unique_lock lk{stream_writes_mutex};
    const VAddr end = address + size;
    std::vector<u64> overlapping;
    for (const auto& write : stream_writes) {
        if (address < write.end && write.begin < end) {
            overlapping.push_back(write.seq);
        }
    }
    if (overlapping.empty()) {
        return 0;
    }
    const u64 last = overlapping.back();
    ++stream_write_waiters;
    stream_writes_landed.wait(
        lk, [&] { return stream_writes.empty() || stream_writes.front().seq > last; });
    --stream_write_waiters;
    for (const u64 seq : overlapping) {
        const auto& landed = stream_writes_history[seq % stream_writes_history.size()];
        ASSERT_MSG(landed.seq == seq, "macOS: stream write history overrun while waiting");
        ASSERT_MSG(!landed.through_gpu,
                   "macOS: command memory {:#x}+{:#x} is submitted after a DMA_DATA into it that "
                   "runs on the GPU; the copy taken at submit cannot include it (not implemented)",
                   address, size);
    }
    return static_cast<u32>(overlapping.size());
}

namespace {
std::mutex builder_trace_mutex;
std::array<BuilderTrace::Call, 8192> builder_trace;
u64 builder_trace_calls = 0;
std::atomic<u32> builder_trace_threads = 0;
} // namespace

u32 BuilderTrace::ThreadNumber() {
    static thread_local const u32 thread = ++builder_trace_threads;
    return thread;
}

void BuilderTrace::Note(const char* builder, const u32* cmdbuf, u32 size) {
    const u32 thread = ThreadNumber();
    std::scoped_lock lk{builder_trace_mutex};
    const u64 seq = builder_trace_calls++;
    builder_trace[seq % builder_trace.size()] = {builder, cmdbuf, size, thread, seq};
}

namespace {
struct SubmittedRegion {
    const u32* guest;
    u32 size_dw;
    u64 submit_number;
    u32 thread;
};
std::mutex recent_regions_mutex;
std::deque<SubmittedRegion> recent_regions; ///< oldest first
} // namespace

u32 CmdSnapshot::ReportRecentOverlaps() {
    constexpr u64 Recent = 64; // submissions
    const u32 thread = BuilderTrace::ThreadNumber();
    std::scoped_lock lk{recent_regions_mutex};
    while (!recent_regions.empty() &&
           recent_regions.front().submit_number + Recent < submit_number) {
        recent_regions.pop_front();
    }
    u32 overlaps = 0;
    for (const auto& region : regions) {
        if (!region.copied) {
            continue;
        }
        const u32* end = region.guest + region.size_dw;
        for (const auto& old : recent_regions) {
            const u32* old_end = old.guest + old.size_dw;
            // The same start is one buffer submitted again (called again, or a per-frame buffer
            // at a fixed address with a different length): not two writers.
            if (old.guest == region.guest || !(region.guest < old_end && old.guest < end)) {
                continue;
            }
            ++overlaps;
            static std::atomic<u64> reports = 0;
            if (const u64 n = ++reports; n <= 16 || (n & 1023) == 0) {
                LOG_CRITICAL(Render,
                             "CMD-OVERLAP #{}: submission {} (thread {}) buffer {}+{} dwords "
                             "partly overlaps submission {} (thread {}) buffer {}+{} dwords{}",
                             n, submit_number, thread, fmt::ptr(region.guest), region.size_dw,
                             old.submit_number, old.thread, fmt::ptr(old.guest), old.size_dw,
                             thread == old.thread ? "" : " (different threads)");
            }
        }
    }
    for (const auto& region : regions) {
        if (region.copied) {
            recent_regions.push_back({region.guest, region.size_dw, submit_number, thread});
        }
    }
    return overlaps;
}

std::vector<BuilderTrace::Call> BuilderTrace::Around(const u32* begin, const u32* end, u32 last) {
    std::scoped_lock lk{builder_trace_mutex};
    std::vector<Call> calls;
    const u64 count = std::min<u64>(builder_trace_calls, builder_trace.size());
    for (u64 seq = builder_trace_calls - count; seq < builder_trace_calls; ++seq) {
        const auto& call = builder_trace[seq % builder_trace.size()];
        if ((call.cmdbuf < end && begin < call.cmdbuf + call.size) ||
            seq + last >= builder_trace_calls) {
            calls.push_back(call);
        }
    }
    return calls;
}

void BuilderTrace::Report(const u32* at) {
    constexpr u32 Window = 64; // dwords on each side
    LOG_CRITICAL(Render, "Builder calls writing within {} dwords of the malformed packet at {} "
                         "(and the 8 latest), oldest first:",
                 Window, fmt::ptr(at));
    for (const auto& call : Around(at - Window, at + Window, 8)) {
        LOG_CRITICAL(Render, "  #{} thread {} {} cmdbuf {} ({:+} dwords from the packet) size {}",
                     call.seq, call.thread, call.builder, fmt::ptr(call.cmdbuf),
                     (std::bit_cast<intptr_t>(call.cmdbuf) - std::bit_cast<intptr_t>(at)) /
                         intptr_t{sizeof(u32)},
                     call.size);
    }
}

std::shared_ptr<CmdSnapshot> CmdSnapshot::Take(std::span<const u32> dcb,
                                               std::span<const u32> ccb) {
    const auto start = std::chrono::steady_clock::now();
    auto snapshot = std::make_shared<CmdSnapshot>();
    auto& words = snapshot->words;
    auto& regions = snapshot->regions;
    const auto add = [&](const u32* guest, u32 size_dw, bool constant, bool submitted) {
        Region region{.guest = guest,
                      .size_dw = size_dw,
                      .copy_offset = static_cast<u32>(words.size()),
                      .constant = constant,
                      .copied = true};
        // The command processor fetches this after executing the stream writes parsed before
        // the submit; the ones still on their way to guest memory land first.
        snapshot->writes_waited +=
            StreamWrites::WaitLanded(reinterpret_cast<VAddr>(guest), size_t(size_dw) * sizeof(u32));
        words.resize(words.size() + size_dw);
        if (submitted) {
            // The guest handed these buffers over: reading them cannot legitimately fault.
            std::memcpy(words.data() + region.copy_offset, guest, size_t(size_dw) * sizeof(u32));
        } else if (!CopyGuest(words.data() + region.copy_offset, guest, size_dw)) {
            region.copied = false;
            words.resize(region.copy_offset);
        }
        regions.push_back(region);
    };
    add(dcb.data(), static_cast<u32>(dcb.size()), false, true);
    if (!ccb.empty()) {
        add(ccb.data(), static_cast<u32>(ccb.size()), true, true);
        snapshot->has_ccb = true;
    }

    // Every captured region is walked once; indirect buffers found in it are appended.
    for (size_t i = 0; i < regions.size(); ++i) {
        if (!regions[i].copied) {
            continue;
        }
        const bool constant = regions[i].constant;
        const u32 base = regions[i].copy_offset;
        const u32 size = regions[i].size_dw;
        const auto ib_opcode =
            constant ? PM4ItOpcode::IndirectBufferConst : PM4ItOpcode::IndirectBuffer;
        for (u32 at = 0; at < size;) {
            const auto* header = reinterpret_cast<const PM4Header*>(words.data() + base + at);
            if (header->type == 2) {
                ++at;
                continue;
            }
            if (header->type != 3) {
                break; // the parser reports a malformed stream when it gets here
            }
            const u32 length = header->type3.NumWords() + 1;
            if (length > size - at) {
                break;
            }
            if (header->type3.opcode == ib_opcode &&
                length * sizeof(u32) >= sizeof(PM4CmdIndirectBuffer)) {
                const auto* ib = reinterpret_cast<const PM4CmdIndirectBuffer*>(header);
                const u32* target = ib->Address<const u32>();
                const u32 ib_size = ib->ib_size;
                const bool chain = ib->chain.Value();
                if (!snapshot->Find(target, ib_size, constant)) {
                    ASSERT_MSG(regions.size() < MaxRegions && words.size() + ib_size <= MaxWords,
                               "macOS: command snapshot of submission at {} exceeds {} buffers / "
                               "{} MiB",
                               fmt::ptr(dcb.data()), MaxRegions, (MaxWords * 4) >> 20);
                    add(target, ib_size, constant, false); // `header` is stale from here on
                }
                if (chain) {
                    break; // a chained IB is a jump: the rest of this buffer is never executed
                }
            }
            at += length;
        }
    }

    if (snapshot->writes_waited) {
        static std::atomic<u64> waits = 0;
        if (const u64 n = ++waits; n <= 8 || (n & 1023) == 0) {
            LOG_INFO(Render,
                     "Command snapshot #{} of submission at {} waited for {} stream writes into "
                     "its command memory",
                     n, fmt::ptr(dcb.data()), snapshot->writes_waited);
        }
    }
    snapshot->copied_bytes = words.size() * sizeof(u32);
    snapshot->copy_ns = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                             std::chrono::steady_clock::now() - start)
                                             .count());
    return snapshot;
}

CmdSnapshot::Region* CmdSnapshot::Find(const u32* guest, u32 size_dw, bool constant) {
    for (auto& region : regions) {
        if (region.guest == guest && region.size_dw == size_dw && region.constant == constant) {
            return &region;
        }
    }
    return nullptr;
}

bool CmdSnapshot::OverlapsUnparsed(VAddr address, u64 size) const {
    for (const auto& region : regions) {
        if (!region.copied || region.parsed_dw >= region.size_dw) {
            continue;
        }
        const VAddr begin = reinterpret_cast<VAddr>(region.guest + region.parsed_dw);
        const VAddr end = reinterpret_cast<VAddr>(region.guest + region.size_dw);
        if (address < end && address + size > begin) {
            return true;
        }
    }
    return false;
}

u32 CmdSnapshot::ReportGuestRewrites(const Region& region) const {
    if (!region.copied) {
        return 0;
    }
    std::vector<u32> live(region.size_dw);
    if (!CopyGuest(live.data(), region.guest, region.size_dw)) {
        LOG_CRITICAL(Render, "CMD-SNAPSHOT submission {} buffer {} ({} dwords) is no longer readable",
                     submit_number, fmt::ptr(region.guest), region.size_dw);
        return region.size_dw;
    }
    const auto copy = Words(region);
    u32 differing = 0;
    for (u32 i = 0; i < region.size_dw; ++i) {
        if (copy[i] == live[i]) {
            continue;
        }
        if (differing < 4) {
            LOG_CRITICAL(Render,
                         "CMD-SNAPSHOT guest rewrote submitted command memory before it was "
                         "parsed: submission {} buffer {} dword {} submitted {:#010x} now {:#010x}",
                         submit_number, fmt::ptr(region.guest), i, copy[i], live[i]);
        }
        ++differing;
    }
    if (differing) {
        LOG_CRITICAL(Render, "CMD-SNAPSHOT submission {} buffer {}: {} of {} dwords differ",
                     submit_number, fmt::ptr(region.guest), differing, region.size_dw);
    }
    return differing;
}

} // namespace AmdGpu
