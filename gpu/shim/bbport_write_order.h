// bbport (macOS): the ordering an in-order WRITE_DATA keeps with labels at GPU completion.
//
// On the PS4 the CP writes WRITE_DATA when it parses it, ahead of the pipeline: earlier EOP/EOS
// labels (end of pipe) may land after it. But a WAIT_REG_MEM between them blocks the CP until
// its value is there, so a WRITE_DATA after a met WAIT_REG_MEM lands after the label it waited
// for. Stage A meets some WAIT_REG_MEMs early, from a fence value still pending (PendingFence-
// Value): it records the deferred signals issued at that point, and later WRITE_DATAs wait for
// those (Astra-2's review: the guest's EOS/EOP -> WAIT_REG_MEM helpers 0x26b08e0/0x26b09e0).
#pragma once
#include <atomic>
#include <cstdint>

namespace BbWriteOrder {

/// The deferred signals (by issue count) that forwarded waits let the stream pass. Monotonic:
/// a WRITE_DATA reading a later value than its own position needed only waits longer.
inline std::atomic<std::uint64_t> forwarded_through{0};

/// Stage A met a WAIT_REG_MEM from a pending value, with `issued` deferred signals issued.
inline void NoteForwardedWait(std::uint64_t issued) {
    std::uint64_t now = forwarded_through.load(std::memory_order_relaxed);
    while (issued > now && !forwarded_through.compare_exchange_weak(
                               now, issued, std::memory_order_release, std::memory_order_relaxed)) {
    }
}

/// The issue count a WRITE_DATA must wait to see written (0: none).
inline std::uint64_t Required() {
    return forwarded_through.load(std::memory_order_acquire);
}

} // namespace BbWriteOrder
