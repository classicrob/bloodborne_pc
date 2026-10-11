// bbport (macOS): the counts behind the scheduler's deferred guest-visible writes, by kind.
// Recorder-side writes (SignalAfterHostCopies: host copies, RecorderFences) and completion
// labels (SignalAtGpuCompletion) each have their own issued/completed counters, so neither
// is derived by subtracting the other from a total that two threads update independently
// (docs/macos/throttle-relaxation.md, R0). Issued by the one thread recording at a time;
// completed from the copy threads (recorder) or the label writer (labels).
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>

namespace BbDeferred {

struct Count {
    std::atomic<std::uint64_t> issued{0};
    /// Shared: completion callbacks may outlive a moved-from owner.
    std::shared_ptr<std::atomic<std::uint64_t>> done =
        std::make_shared<std::atomic<std::uint64_t>>(0);

    std::uint64_t Issue() {
        return issued.fetch_add(1, std::memory_order_relaxed) + 1;
    }
    void Complete(std::uint64_t n = 1) {
        done->fetch_add(n, std::memory_order_release);
    }
    /// Every one issued up to `through` has completed.
    [[nodiscard]] bool CompleteThrough(std::uint64_t through) const {
        return done->load(std::memory_order_acquire) >= through;
    }
    [[nodiscard]] std::uint64_t Issued() const {
        return issued.load(std::memory_order_relaxed);
    }
};

} // namespace BbDeferred
