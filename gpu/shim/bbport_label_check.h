// bbport (macOS diagnostic, BB_LABEL_CHECK=1): did the guest act on a frame's work as done before
// its end-of-pipe label was written?
//
// The guest frees per-frame objects that hold EOP labels (48-byte heap blocks, label at offset
// 0) once it believes the GPU is done. A label written after that lands in freed memory, and the
// allocator's free-list link at offset 0 gets the label value (Guest fault 0x263b8e7,
// docs/history/SESSION_2026-09-30.md). So: the value at a label address is read when its EOP is
// parsed, and again just before the label is written. If the guest changed it in between, it freed
// or reused the object (or reset the label) before the GPU signalled. Each such write is reported
// with the values and the time it was pending. Any change of when the guest is released from the
// submitDone throttle must keep this at zero.
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_map>

namespace BbLabelCheck {

inline bool Enabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_LABEL_CHECK");
        return env && env[0] == '1';
    }();
    return enabled;
}

struct Pending {
    std::uint64_t observed;
    std::chrono::steady_clock::time_point parsed;
};

struct Written {
    std::uint64_t value;
    std::chrono::steady_clock::time_point when;
};

inline std::mutex mutex;
inline std::unordered_map<std::uint64_t, std::deque<Pending>> pending; ///< by label address
/// The last value an EOP wrote to each label still pending: with frames in flight, a later EOP
/// on the same label finds the earlier one's value there, which is the GPU's, not the guest's.
inline std::unordered_map<std::uint64_t, Written> written;
inline std::atomic<std::uint64_t> reports{0}, checked{0};

inline std::uint64_t Read(std::uint64_t address, std::uint32_t num_bytes) {
    std::uint64_t value = 0;
    std::memcpy(&value, reinterpret_cast<const void*>(address), num_bytes < 8 ? num_bytes : 8);
    return value;
}

/// When the EOP is parsed, before it is handed to the recording thread (which may write it at
/// once).
inline void NoteParsed(std::uint64_t address, std::uint32_t num_bytes) {
    const std::uint64_t value = Read(address, num_bytes);
    std::scoped_lock lk{mutex};
    pending[address].push_back({value, std::chrono::steady_clock::now()});
}

/// Just before the label is written with `data`. Returns true if the guest changed it since the
/// matching NoteParsed (reported on stderr: first 16, then every 1024th).
inline bool BeforeWrite(std::uint64_t address, std::uint64_t data, std::uint32_t num_bytes) {
    const std::uint64_t current = Read(address, num_bytes);
    const std::uint64_t mask =
        num_bytes >= 8 ? ~std::uint64_t{0} : (std::uint64_t{1} << (8 * num_bytes)) - 1;
    Pending entry;
    bool ours = false;
    {
        std::scoped_lock lk{mutex};
        const auto it = pending.find(address);
        if (it == pending.end() || it->second.empty()) {
            return false; // parsed before the check was on, or written without a parse note
        }
        entry = it->second.front();
        it->second.pop_front();
        if (const auto last = written.find(address); last != written.end()) {
            ours = last->second.when > entry.parsed && current == last->second.value;
        }
        if (it->second.empty()) {
            pending.erase(it);
            written.erase(address);
        } else {
            written[address] = {data & mask, std::chrono::steady_clock::now()};
        }
    }
    checked.fetch_add(1, std::memory_order_relaxed);
    if (current == entry.observed || ours) {
        return false;
    }
    const std::uint64_t n = reports.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 16 || (n & 1023) == 0) {
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - entry.parsed)
                              .count();
        std::uint64_t block[6]{};
        std::memcpy(block, reinterpret_cast<const void*>(address & ~std::uint64_t{15}),
                    sizeof(block));
        std::fprintf(stderr,
                     "LABEL-CHECK #%llu: label %#llx changed by the guest before its EOP wrote it: "
                     "%#llx when parsed, %#llx now, writing %#llx (%u bytes, pending %.1f ms); "
                     "block %#llx %#llx %#llx %#llx %#llx %#llx\n",
                     static_cast<unsigned long long>(n), static_cast<unsigned long long>(address),
                     static_cast<unsigned long long>(entry.observed),
                     static_cast<unsigned long long>(current),
                     static_cast<unsigned long long>(data), num_bytes, ms,
                     static_cast<unsigned long long>(block[0]),
                     static_cast<unsigned long long>(block[1]),
                     static_cast<unsigned long long>(block[2]),
                     static_cast<unsigned long long>(block[3]),
                     static_cast<unsigned long long>(block[4]),
                     static_cast<unsigned long long>(block[5]));
    }
    return true;
}

} // namespace BbLabelCheck
