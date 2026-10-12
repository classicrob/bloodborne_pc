// bbport (macOS): the ordering an in-order WRITE_DATA keeps with labels at GPU completion.
//
// On the PS4 the CP writes WRITE_DATA when it parses it, ahead of the pipeline: earlier EOP/EOS
// labels (end of pipe) may land after it. But a WAIT_REG_MEM between them blocks the CP until
// its value is there, so a WRITE_DATA after a met WAIT_REG_MEM lands after the label it waited
// for. Stage A meets some WAIT_REG_MEMs early, from a fence value still pending (PendingFence-
// Value), whose producing EOP/EOS stage B may not have issued yet. So the dependency is taken
// at the WAIT's position in the stream: an in-order task there (after the producer was issued)
// records the completion labels issued so far, and every later in-order WRITE_DATA waits until
// those are written (Astra-2's review; the guest's EOS/EOP -> WAIT_REG_MEM helpers
// 0x26b08e0/0x26b09e0).
//
// The count is of completion labels only: their one writer completes them in issue order, so
// "N written" means the first N (a prefix). Recorder-side writes, which complete independently,
// are waited for separately (WaitRecorderSignals).
#pragma once
#include <atomic>
#include <cstdint>

namespace BbWriteOrder {

/// One scheduler's barrier (its label counts restart with it): the completion labels, by issue
/// count, that forwarded waits stand for. Monotonic: a WRITE_DATA that reads a later value than
/// its own position needed only waits longer.
class Barrier {
public:
    /// In stream order, at a WAIT_REG_MEM stage A met early: `labels_issued` completion labels
    /// were issued before it.
    void AtForwardedWait(std::uint64_t labels_issued) {
        std::uint64_t now = labels_through.load(std::memory_order_relaxed);
        while (labels_issued > now &&
               !labels_through.compare_exchange_weak(now, labels_issued,
                                                     std::memory_order_release,
                                                     std::memory_order_relaxed)) {
        }
    }

    /// The completion labels a WRITE_DATA must see written (0: none).
    [[nodiscard]] std::uint64_t Required() const {
        return labels_through.load(std::memory_order_acquire);
    }

    /// Before an in-order WRITE_DATA: `wait_labels_through(n)` waits until the first n
    /// completion labels are written, when a forwarded wait requires it.
    template <class WaitLabelsThrough>
    void BeforeWriteData(WaitLabelsThrough&& wait_labels_through) const {
        if (const std::uint64_t required = Required()) {
            wait_labels_through(required);
        }
    }

private:
    std::atomic<std::uint64_t> labels_through{0};
};

} // namespace BbWriteOrder
