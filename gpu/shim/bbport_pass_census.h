// bbport (macOS): render passes per frame and what ends each, for cutting Metal encoding cost
// (each render pass is a MTLRenderCommandEncoder; PREVIEW-20 prefill A/B: begin/end encoding
// 8-14 ms per frame). Printed with the parse split, every 600 frame ends ("Render passes, last
// 600 frame ends"). An ender that is not one of the named causes is listed by its caller's
// address as a library offset ("lib+0x..."), for atos against the same libbbgpu.dylib.
// Submissions per frame and their causes are PM4's Flush census in the parse-split block.
// Always on: a few stores per pass under a mutex (hundreds per frame).
#pragma once
#ifdef __APPLE__
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <dlfcn.h>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace BbPassCensus {

enum class Why : std::uint32_t {
    TargetChange, ///< BeginRendering with other attachments or extent
    Submit,       ///< the submission ends (SubmitExecution: Flush, Finish)
    Barrier,      ///< Runtime::FlushBarriers (layout transitions, memory barriers)
    Other,        ///< any other EndRendering caller (copies, clears, compute): by caller
    Count
};
inline constexpr const char* why_names[] = {"target change", "submit", "barrier", "other"};

namespace detail {
/// Caller address -> count, a small open-addressed table (cleared every window).
struct Callers {
    static constexpr std::size_t Size = 256;
    std::array<std::pair<const void*, std::uint64_t>, Size> slots{};
    std::uint64_t dropped = 0;
    void Add(const void* caller) {
        std::size_t i = (reinterpret_cast<std::uintptr_t>(caller) >> 2) % Size;
        for (std::size_t probe = 0; probe < Size; ++probe, i = (i + 1) % Size) {
            if (slots[i].first == caller || !slots[i].first) {
                slots[i] = {caller, slots[i].second + 1};
                return;
            }
        }
        ++dropped;
    }
    void Clear() {
        slots.fill({});
        dropped = 0;
    }
};
inline std::mutex mutex;
inline std::uint64_t begins = 0;
inline std::array<std::uint64_t, std::size_t(Why::Count)> ends{};
inline Callers other_ends;
inline std::uint64_t frames = 0;

inline std::string Offset(const void* address) {
    Dl_info info{};
    char text[64];
    if (dladdr(address, &info) && info.dli_fbase) {
        std::snprintf(text, sizeof(text), "lib+%#lx",
                      static_cast<unsigned long>(reinterpret_cast<std::uintptr_t>(address) -
                                                 reinterpret_cast<std::uintptr_t>(info.dli_fbase)));
    } else {
        std::snprintf(text, sizeof(text), "%p", address);
    }
    return text;
}

/// The top `n` callers as "lib+0x.. x.x/frame", most frequent first.
inline std::string Top(const Callers& callers, std::size_t n, double frames) {
    std::vector<std::pair<const void*, std::uint64_t>> used;
    for (const auto& slot : callers.slots) {
        if (slot.first) {
            used.push_back(slot);
        }
    }
    std::ranges::sort(used, [](const auto& a, const auto& b) { return a.second > b.second; });
    std::string out;
    char item[96];
    for (std::size_t i = 0; i < std::min(n, used.size()); ++i) {
        std::snprintf(item, sizeof(item), "%s%s %.1f", out.empty() ? "" : ", ",
                      Offset(used[i].first).c_str(), used[i].second / frames);
        out += item;
    }
    if (callers.dropped) {
        std::snprintf(item, sizeof(item), "; %llu not tabled",
                      static_cast<unsigned long long>(callers.dropped));
        out += item;
    }
    return out.empty() ? "none" : out;
}
} // namespace detail

inline void NoteBegin() {
    std::scoped_lock lk{detail::mutex};
    ++detail::begins;
}

inline void NoteEnd(Why why, const void* caller) {
    std::scoped_lock lk{detail::mutex};
    ++detail::ends[std::size_t(why)];
    if (why == Why::Other) {
        detail::other_ends.Add(caller);
    }
}

/// On stage A at each frame end: every 600, prints the census and starts a new window.
inline void NoteFrameEnd() {
    std::scoped_lock lk{detail::mutex};
    if (++detail::frames < 600) {
        return;
    }
    const double f = double(detail::frames);
    std::string ends;
    char item[64];
    for (std::size_t i = 0; i < detail::ends.size(); ++i) {
        std::snprintf(item, sizeof(item), "%s%s %.1f", i ? ", " : "", why_names[i],
                      detail::ends[i] / f);
        ends += item;
    }
    std::printf("Render passes, last 600 frame ends: %.1f begun per frame; ended per frame by: "
                "%s\n"
                "  other enders per frame (atos -o libbbgpu.dylib -l 0): %s\n",
                detail::begins / f, ends.c_str(), detail::Top(detail::other_ends, 12, f).c_str());
    std::fflush(stdout);
    detail::begins = detail::frames = 0;
    detail::ends.fill(0);
    detail::other_ends.Clear();
}

} // namespace BbPassCensus
#endif
