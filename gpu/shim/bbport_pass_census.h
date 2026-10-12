// bbport (macOS): render passes per frame and what ends each, for cutting Metal encoding cost
// (each render pass is a MTLRenderCommandEncoder; PREVIEW-20 prefill A/B: begin/end encoding
// 8-14 ms per frame). Printed with the parse split, every 600 frame ends ("Render passes, last
// 600 frame ends"). An ender that is not one of the named causes is listed by its caller's
// address as a library offset ("lib+0x..."), for atos against the same libbbgpu.dylib. Barrier
// ends are attributed by what they wait for (source stage classes, image transitions) and by the
// FlushBarriers caller ("unknown" when no source is attributed); begins whose state equals the
// state of the pass just ended are same-state resume candidates: the targets did not require the
// split, but its barrier may still have been needed (speed lever a, RENDER-PASS-SPLITS). They
// are attributed to what ended the pass before them (the cause, and the ender's or flusher's
// address): the splits that moving that op or barrier out of the pass could save.
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

/// What a barrier that ends a pass waits for (Runtime::FlushBarriers): the source stages of the
/// pending memory hazard, by class, and whether image layout transitions are pending.
enum BarrierSource : std::uint32_t {
    SrcTransfer = 1,  ///< copies, fills, clears, uploads (transfer stages)
    SrcCompute = 2,   ///< compute dispatches (guest, HLE, emulation kernels)
    SrcGraphics = 4,  ///< earlier draws (vertex/fragment/attachment stages)
    SrcOther = 8,     ///< any other stage (host, all-commands)
    SrcImage = 16,    ///< image layout transitions
    SrcCount = 32,
};
inline constexpr const char* source_names[] = {"transfer", "compute", "graphics", "other", "image"};

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
inline std::array<std::uint64_t, SrcCount> barrier_sources{};
inline Callers barrier_callers;
inline std::uint64_t resumes = 0;
inline std::array<std::uint64_t, std::size_t(Why::Count)> resumes_after{};
inline Callers resume_callers;
/// The last pass end: its cause and the ender (Other) or barrier flusher (Barrier) address.
inline Why last_why = Why::Count;
inline const void* last_caller = nullptr;
inline const void* barrier_caller = nullptr; ///< NoteBarrierEnd's flusher, for the NoteEnd next

/// "transfer+image 12.3, compute 4.5": the nonzero source combinations, most frequent first.
inline std::string Sources(double frames) {
    std::vector<std::pair<std::uint32_t, std::uint64_t>> used;
    for (std::uint32_t mask = 0; mask < SrcCount; ++mask) {
        if (barrier_sources[mask]) {
            used.emplace_back(mask, barrier_sources[mask]);
        }
    }
    std::ranges::sort(used, [](const auto& a, const auto& b) { return a.second > b.second; });
    std::string out;
    char item[96];
    for (const auto& [mask, count] : used) {
        std::string name;
        for (std::uint32_t bit = 0; bit < 5; ++bit) {
            if (mask & (1u << bit)) {
                name += (name.empty() ? "" : "+") + std::string(source_names[bit]);
            }
        }
        std::snprintf(item, sizeof(item), "%s%s %.1f", out.empty() ? "" : ", ",
                      name.empty() ? "unknown" : name.c_str(), count / frames);
        out += item;
    }
    return out.empty() ? "none" : out;
}

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

/// A pass begins; `resumed`: with exactly the state of the pass that ended just before it (same
/// attachments, layouts, clears and extent): a same-state resume candidate. Whether the split was
/// avoidable depends on the barrier or op between them, which this does not decide.
inline void NoteBegin(bool resumed) {
    std::scoped_lock lk{detail::mutex};
    ++detail::begins;
    if (!resumed) {
        return;
    }
    ++detail::resumes;
    if (detail::last_why != Why::Count) {
        ++detail::resumes_after[std::size_t(detail::last_why)];
    }
    if (detail::last_caller) {
        detail::resume_callers.Add(detail::last_caller);
    }
}

/// A barrier is about to end an open pass: what it waits for (BarrierSource bits) and who flushed.
inline void NoteBarrierEnd(std::uint32_t sources, const void* caller) {
    std::scoped_lock lk{detail::mutex};
    ++detail::barrier_sources[sources % SrcCount];
    detail::barrier_callers.Add(caller);
    detail::barrier_caller = caller;
}

inline void NoteEnd(Why why, const void* caller) {
    std::scoped_lock lk{detail::mutex};
    ++detail::ends[std::size_t(why)];
    if (why == Why::Other) {
        detail::other_ends.Add(caller);
    }
    detail::last_why = why;
    detail::last_caller = why == Why::Other     ? caller
                          : why == Why::Barrier ? detail::barrier_caller
                                                : nullptr;
    detail::barrier_caller = nullptr;
}

/// On stage A at each frame end: every 600, prints the census and starts a new window.
inline void NoteFrameEnd() {
    std::scoped_lock lk{detail::mutex};
    if (++detail::frames < 600) {
        return;
    }
    const double f = double(detail::frames);
    std::string ends, resumes_after;
    char item[64];
    for (std::size_t i = 0; i < detail::ends.size(); ++i) {
        std::snprintf(item, sizeof(item), "%s%s %.1f", i ? ", " : "", why_names[i],
                      detail::ends[i] / f);
        ends += item;
        std::snprintf(item, sizeof(item), "%s%s %.1f", i ? ", " : "", why_names[i],
                      detail::resumes_after[i] / f);
        resumes_after += item;
    }
    std::printf("Render passes, last 600 frame ends: %.1f begun per frame; ended per frame by: "
                "%s\n"
                "  other enders per frame (atos -o libbbgpu.dylib -l 0): %s\n"
                "  barrier ends per frame by what they wait for: %s; by flusher: %s\n"
                "  same-state resume candidates per frame (begun with the state the last pass "
                "ended with): %.1f; after a pass ended by: %s; by its ender or flusher: %s\n",
                detail::begins / f, ends.c_str(), detail::Top(detail::other_ends, 12, f).c_str(),
                detail::Sources(f).c_str(), detail::Top(detail::barrier_callers, 8, f).c_str(),
                detail::resumes / f, resumes_after.c_str(),
                detail::Top(detail::resume_callers, 8, f).c_str());
    std::fflush(stdout);
    detail::begins = detail::frames = detail::resumes = 0;
    detail::ends.fill(0);
    detail::other_ends.Clear();
    detail::barrier_sources.fill(0);
    detail::barrier_callers.Clear();
    detail::resumes_after.fill(0);
    detail::resume_callers.Clear();
}

} // namespace BbPassCensus
#endif
