// bbport (macOS): how far each side of the flip pipeline has got, for stop lines (a parse-time
// read of reused guest data is judged against the guest's reuse gate). Four marks, each a count
// and (where known) the latest flip argument:
// - attempted: the guest called a flip API (sceGnmSubmitAndFlip..., or a CPU flip), at entry,
//   before validation and admission;
// - parsed: the GPU command thread (stage A) parsed a flip packet (no argument there);
// - prepared: flip preparation entered (SubmitFlipInternal: on the draw recording thread for EOP
//   flips, as a host command for CPU flips);
// - published: the flip status was applied for the guest (before its event and label effects).
// Each field is sampled on its own and concurrent producers can reorder: approximate
// observations, not a coherent snapshot or a monotonic argument.
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

inline Mark attempted, parsed, prepared, published;

/// The marks as they stand, read one after another (approximate).
inline std::string Describe() {
    char text[320];
    std::snprintf(text, sizeof(text),
                  "; flips at the stop (approximate): guest API calls %llu (latest arg %lld), "
                  "flip packets parsed %llu, flip preparation entered %llu (latest arg %lld), "
                  "status published %llu (latest arg %lld)",
                  static_cast<unsigned long long>(attempted.count.load()),
                  static_cast<long long>(attempted.arg.load()),
                  static_cast<unsigned long long>(parsed.count.load()),
                  static_cast<unsigned long long>(prepared.count.load()),
                  static_cast<long long>(prepared.arg.load()),
                  static_cast<unsigned long long>(published.count.load()),
                  static_cast<long long>(published.arg.load()));
    return text;
}

} // namespace BbFrameMarks
