// bbport (macOS): compute-queue SET_SH_REG writes that land outside the queue's compute program
// state. The parser applies those to the one shared register file, i.e. into the graphics SH
// registers for offsets below 0x200 (as upstream does); on the PS4 a compute ring cannot change
// the graphics pipe's user data. Counted always, the first ones logged, and stated in stop
// lines, to settle whether this happens in the game before changing it.
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>

namespace BbComputeSh {

/// Graphics stage user-data blocks (SPI_SHADER_USER_DATA_{PS,VS,GS,ES,HS,LS}_0..15).
inline bool IsGraphicsUserData(std::uint32_t offset) {
    for (const std::uint32_t base : {0x0Cu, 0x4Cu, 0x8Cu, 0xCCu, 0x10Cu, 0x14Cu}) {
        if (offset >= base && offset < base + 16) {
            return true;
        }
    }
    return false;
}

/// Writes (packets) into the graphics SH range, of them touching graphics user data, and
/// writes above the compute program range. Written by the GPU command thread only.
inline std::atomic<std::uint64_t> into_graphics{0}, into_user_data{0}, above_compute{0};
/// The latest write into graphics user data: queue, first offset, words, first value.
inline std::atomic<std::int64_t> last_queue{-1};
inline std::atomic<std::uint32_t> last_offset{0}, last_words{0}, last_value{0};

inline void Add(std::atomic<std::uint64_t>& counter) {
    counter.store(counter.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}

/// One compute SET_SH_REG outside the compute program: `offset` the first SH word, `words`
/// written, `values` the payload. Logs the first 8 of each kind.
inline void Note(std::uint32_t queue, std::uint32_t offset, std::uint32_t words,
                 const std::uint32_t* values) {
    if (offset >= 0x200) {
        if (above_compute.load(std::memory_order_relaxed) < 8) {
            std::printf("Compute SET_SH_REG: queue %u writes %u word(s) at SH %#x, above the "
                        "compute program (first %#x)\n",
                        queue, words, offset, words ? values[0] : 0);
            std::fflush(stdout);
        }
        Add(above_compute);
        return;
    }
    bool user_data = false;
    for (std::uint32_t i = 0; i < words; ++i) {
        user_data = user_data || IsGraphicsUserData(offset + i);
    }
    if (into_graphics.load(std::memory_order_relaxed) < 8) {
        std::printf("Compute SET_SH_REG: queue %u writes %u word(s) at SH %#x into the GRAPHICS "
                    "registers%s (first %#x)\n",
                    queue, words, offset, user_data ? ", graphics USER DATA" : "",
                    words ? values[0] : 0);
        std::fflush(stdout);
    }
    Add(into_graphics);
    if (user_data) {
        Add(into_user_data);
        last_queue.store(queue, std::memory_order_relaxed);
        last_offset.store(offset, std::memory_order_relaxed);
        last_words.store(words, std::memory_order_relaxed);
        last_value.store(words ? values[0] : 0, std::memory_order_relaxed);
    }
}

/// For summaries and stop lines.
inline std::string Describe() {
    char text[256];
    std::snprintf(text, sizeof(text),
                  "compute SET_SH_REG writes into graphics SH registers %llu (into graphics user "
                  "data %llu, latest: queue %lld, SH %#x, %u word(s), first %#x), above the "
                  "compute program %llu",
                  static_cast<unsigned long long>(into_graphics.load()),
                  static_cast<unsigned long long>(into_user_data.load()),
                  static_cast<long long>(last_queue.load()), last_offset.load(),
                  last_words.load(), last_value.load(),
                  static_cast<unsigned long long>(above_compute.load()));
    return text;
}

} // namespace BbComputeSh
