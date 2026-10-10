// SPDX-License-Identifier: GPL-2.0-or-later
// bbport (macOS): submit-time copies of graphics command memory (cmd_snapshot.h).

#include <chrono>
#include <csetjmp>
#include <cstring>
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

} // namespace

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
                if (!snapshot->Find(target, ib_size, constant)) {
                    ASSERT_MSG(regions.size() < MaxRegions && words.size() + ib_size <= MaxWords,
                               "macOS: command snapshot of submission at {} exceeds {} buffers / "
                               "{} MiB",
                               fmt::ptr(dcb.data()), MaxRegions, (MaxWords * 4) >> 20);
                    add(target, ib_size, constant, false); // `header` is stale from here on
                }
            }
            at += length;
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
