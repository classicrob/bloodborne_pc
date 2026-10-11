// bbport (macOS): how far each side of the flip pipeline has got, for stop lines (a parse-time
// read of reused guest data is judged against the guest's reuse gate: docs/macos/
// parse-time-table-reads.md). Three monotonic marks, each the count of flips and the newest
// flip argument:
// - submitted: the guest submitted a frame with a flip (sceGnmSubmitAndFlip..., or a CPU flip);
// - reached: the GPU command thread reached that flip (VideoOutDriver::SubmitFlipInternal);
// - published: the guest was told (flip status and event; at GPU retirement by default).
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>

namespace BbFrameMarks {

struct Mark {
    std::atomic<std::uint64_t> count{0};
    std::atomic<std::int64_t> arg{0};
    void Note(std::int64_t flip_arg) {
        arg.store(flip_arg, std::memory_order_relaxed);
        count.fetch_add(1, std::memory_order_release);
    }
};

inline Mark submitted, reached, published;

/// "; flips: guest submitted N (arg a), command thread reached M (arg b), published to the
/// guest P (arg c)": the three marks as they stand (read one after another, not atomically).
inline std::string Describe() {
    char text[224];
    std::snprintf(text, sizeof(text),
                  "; flips at the stop: guest submitted %llu (arg %lld), GPU command thread "
                  "reached %llu (arg %lld), published to the guest %llu (arg %lld)",
                  static_cast<unsigned long long>(submitted.count.load()),
                  static_cast<long long>(submitted.arg.load()),
                  static_cast<unsigned long long>(reached.count.load()),
                  static_cast<long long>(reached.arg.load()),
                  static_cast<unsigned long long>(published.count.load()),
                  static_cast<long long>(published.arg.load()));
    return text;
}

} // namespace BbFrameMarks
