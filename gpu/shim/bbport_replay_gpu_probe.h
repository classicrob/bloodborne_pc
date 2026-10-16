// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#ifdef __APPLE__

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Implemented only by the validated replay driver, never by an environment flag.
// Integration requires the replay owner's bbport_replay_active.h/API commit.
namespace BbReplay { bool MeasurementActive(); }

namespace BbReplayGpuProbe {

enum class Mode { Off, Scissor1px, DiscardLoadStore };

[[noreturn]] inline void Fail(const char* reason) {
    std::fprintf(stderr, "STOP: BB_REPLAY_GPU_PROBE: %s\n", reason);
    std::abort();
}

inline Mode Requested() {
    static const Mode mode = [] {
        const char* value = std::getenv("BB_REPLAY_GPU_PROBE");
        if (!value || !*value || !std::strcmp(value, "off")) return Mode::Off;
        if (!std::strcmp(value, "scissor-1px")) return Mode::Scissor1px;
        if (!std::strcmp(value, "discard-load-store")) return Mode::DiscardLoadStore;
        Fail("expected off, scissor-1px or discard-load-store");
    }();
    return mode;
}

inline const char* Name() {
    switch (Requested()) {
    case Mode::Off: return "off";
    case Mode::Scissor1px: return "scissor-1px";
    case Mode::DiscardLoadStore: return "discard-load-store";
    }
    Fail("invalid mode");
}

inline Mode CheckedMode() {
    const Mode mode = Requested();
    if (mode == Mode::Off) return mode;
    if (!BbReplay::MeasurementActive()) {
        Fail("requires the validated replay measurement loop; ordinary game/setup refused");
    }
    static const bool announced = [] {
        const char* gpu_wait = std::getenv("BB_REPLAY_GPU_WAIT");
        if (!gpu_wait || std::strcmp(gpu_wait, "1")) {
            Fail("requires BB_REPLAY_GPU_WAIT=1 for completed iteration boundaries");
        }
        const char* retired = std::getenv("BB_WORK_RETIRED");
        if (retired && retired[0] == '0') {
            Fail("BB_WORK_RETIRED starting with 0 bypasses recording retirement");
        }
        std::fprintf(stderr, "REPLAY GPU PROBE mode=%s INVALID FOR CORRECTNESS/QUALITY; "
                             "changes contents, side effects and dependencies; "
                             "timing is not a savings bound\n", Name());
        return true;
    }();
    (void)announced;
    return mode;
}

inline std::atomic<std::uint64_t> draws{0}, scissors{0}, passes{0}, aspects{0}, loads{0};

struct Snapshot {
    std::uint64_t draws, scissors, passes, aspects, loads;
};

inline Snapshot ReadCounters() {
    return {draws.load(std::memory_order_relaxed), scissors.load(std::memory_order_relaxed),
            passes.load(std::memory_order_relaxed), aspects.load(std::memory_order_relaxed),
            loads.load(std::memory_order_relaxed)};
}

// Called by the replay driver after its existing iteration completion fence, outside timing.
inline void ReportDelta(unsigned iteration, const Snapshot& before) {
    if (Requested() == Mode::Off) return;
    CheckedMode();
    const auto after = ReadCounters();
    std::fprintf(stderr, "REPLAY GPU PROBE iteration=%u mode=%s rasterizer_draws=%llu "
                         "clamped_scissors=%llu discarded_passes=%llu discarded_store_aspects=%llu "
                         "discarded_load_aspects=%llu INVALID_FOR_CORRECTNESS_QUALITY=1\n",
                 iteration, Name(), (unsigned long long)(after.draws - before.draws),
                 (unsigned long long)(after.scissors - before.scissors),
                 (unsigned long long)(after.passes - before.passes),
                 (unsigned long long)(after.aspects - before.aspects),
                 (unsigned long long)(after.loads - before.loads));
}

template <typename Rectangles>
inline void ApplyScissors(Rectangles& rectangles) {
    if (CheckedMode() != Mode::Scissor1px) return;
    for (auto& rectangle : rectangles) {
        auto& extent = rectangle.extent;
        // Keep an empty guest scissor empty, and keep the original offset. This is a
        // fragment/raster sensitivity probe; vertex/geometry/compute work remains.
        if (!extent.width || !extent.height) continue;
        if (extent.width > 1 || extent.height > 1) {
            scissors.fetch_add(1, std::memory_order_relaxed);
        }
        extent.width = std::min(extent.width, 1u);
        extent.height = std::min(extent.height, 1u);
    }
}

inline void NoteDraw() {
    if (CheckedMode() != Mode::Off) draws.fetch_add(1, std::memory_order_relaxed);
}

inline void NoteDiscardedPass() { passes.fetch_add(1, std::memory_order_relaxed); }
inline void NoteDiscardedAspect(bool loaded) {
    aspects.fetch_add(1, std::memory_order_relaxed);
    if (loaded) loads.fetch_add(1, std::memory_order_relaxed);
}

template <typename Attachment, typename LoadOp, typename StoreOp>
inline void DiscardAttachment(Attachment& attachment, LoadOp load, LoadOp dont_care_load,
                              StoreOp dont_care_store) {
    if (CheckedMode() != Mode::DiscardLoadStore || !attachment.imageView) return;
    const bool loaded = attachment.loadOp == load;
    if (loaded) attachment.loadOp = dont_care_load;
    attachment.storeOp = dont_care_store;
    NoteDiscardedAspect(loaded);
}

} // namespace BbReplayGpuProbe
#endif
