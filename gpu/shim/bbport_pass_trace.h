// bbport (macOS): a per-pass trace for the render pass lead (diagnostic, off by default). The pass
// census (bbport_pass_census.h) counts passes and what ends them; this records each pass of a few
// frames, so that load/store traffic and the fate of every store can be decided offline
// (tools/macos/pass_trace.py).
//
// BB_PASS_TRACE=<first>[+<count>] records the frames between frame ends first and first+count
// (frame ends counted from 1 at the census's NoteFrameEnd, first >= 1, count 1..64, default 1) to
// BB_PASS_TRACE_OUT, as JSON lines written at each traced frame end. A malformed value stops.
// Per pass ("begin"/"end"): its attachments (image uid, format, texel bytes, extent, layout, load
// op; every attachment is stored), the draws in it, and what ended it (the census's cause and
// ender).
// Between and inside passes ("use"): every access REQUESTED through Runtime::Transit (uid,
// layout, access, stage, whole image or not), recorded before the barrier flush that may end the
// open pass, so "requested_in_pass" does not prove the access ran inside that pass. Image uses
// recorded straight into command buffers without Runtime::Transit are not seen.
// Attachments are resolved only for the next pass, from the targets the rasterizer notes for it
// (NoteView, cleared at every begin), and only where the attachment is the guest image's own
// view. Redirected and proxy views, the blit helper's and the object motion attachment are
// unknown (their bytes are not counted). Only the rasterizer's draws are counted. A pass open when
// the trace starts ends with a "partial" end record.
#pragma once
#ifdef __APPLE__
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include "bbport_pass_census.h"

namespace BbPassTrace {

/// The image behind an attachment view.
struct View {
    std::uint64_t uid = 0;
    std::uint32_t format = 0;      ///< VkFormat
    std::uint32_t texel_bytes = 0; ///< bytes per texel (block bytes for compressed formats)
    std::uint32_t width = 0, height = 0, layers = 0, level = 0;
};

namespace detail {
struct Config {
    bool enabled = false;
    std::uint64_t first = 0, count = 1;
    const char* out = nullptr;
};

inline bool ParseDecimal(const char*& c, std::uint64_t& value) {
    if (*c < '0' || *c > '9') {
        return false;
    }
    value = 0;
    for (; *c >= '0' && *c <= '9'; ++c) {
        if (value > 100000000) {
            return false;
        }
        value = value * 10 + std::uint64_t(*c - '0');
    }
    return true;
}

inline const Config& Get() {
    static const Config config = [] {
        Config c;
        const char* env = std::getenv("BB_PASS_TRACE");
        if (!env || !*env) {
            return c;
        }
        const char* p = env;
        bool ok = ParseDecimal(p, c.first) && c.first >= 1;
        if (ok && *p == '+') {
            ++p;
            ok = ParseDecimal(p, c.count) && c.count >= 1 && c.count <= 64;
        }
        if (!ok || *p) {
            std::fprintf(stderr,
                         "STOP: BB_PASS_TRACE=%s: <first frame end, 1..>[+<count 1..64>]\n", env);
            std::abort();
        }
        c.out = std::getenv("BB_PASS_TRACE_OUT");
        if (!c.out || !*c.out) {
            std::fprintf(stderr, "STOP: BB_PASS_TRACE is set without BB_PASS_TRACE_OUT\n");
            std::abort();
        }
        c.enabled = true;
        return c;
    }();
    return config;
}

inline std::mutex mutex;
/// The targets noted for the next pass (view handle, image); cleared at every begin.
inline std::array<std::pair<std::uint64_t, View>, 9> pending{};
inline std::uint32_t num_pending = 0;
inline std::uint64_t frame_ends = 0;
inline bool active = false; ///< inside the traced frames
inline std::FILE* file = nullptr;
inline std::string lines;
inline std::uint64_t pass = 0;  ///< passes begun in the traced frames (the open one, when open)
inline bool open = false;        ///< a pass is open (tracked whenever Enabled())
inline bool open_traced = false; ///< ... and it began inside the traced frames
inline std::uint64_t draws = 0; ///< in the open pass
inline std::uint64_t frame_passes = 0, frame_draws = 0, frame_uses = 0;
/// The attachment views and extent of the last pass begun, for "same targets".
inline std::array<std::uint64_t, 9> last_targets{};
inline std::uint32_t last_width = 0, last_height = 0;

inline void Append(const char* format, auto... args) {
    char line[512];
    const int n = std::snprintf(line, sizeof(line), format, args...);
    lines.append(line, n < 0 ? 0 : std::min<std::size_t>(std::size_t(n), sizeof(line) - 1));
}

inline void AppendAttachment(bool& first, const char* slot, std::uint64_t view,
                             std::uint32_t layout, bool clear) {
    const View* found = nullptr;
    for (std::uint32_t i = 0; i < num_pending; ++i) {
        if (view && pending[i].first == view) {
            found = &pending[i].second;
        }
    }
    const View v = found ? *found : View{};
    Append("%s{\"slot\":\"%s\",\"uid\":%llu,\"known\":%d,\"format\":%u,\"texel_bytes\":%u,"
           "\"width\":%u,\"height\":%u,\"layers\":%u,\"level\":%u,\"layout\":%u,"
           "\"load\":\"%s\"}",
           first ? "" : ",", slot, static_cast<unsigned long long>(v.uid), found ? 1 : 0,
           v.format, v.texel_bytes, v.width, v.height, v.layers,
           v.level, layout, clear ? "clear" : "load");
    first = false;
}
} // namespace detail

[[nodiscard]] inline bool Enabled() {
    return detail::Get().enabled;
}

/// The next pass's targets are about to be noted (the rasterizer's full render state).
inline void ClearViews() {
    std::scoped_lock lk{detail::mutex};
    detail::num_pending = 0;
}

/// The next pass uses `view`, the guest image's own view of `image`. Only when Enabled().
inline void NoteView(std::uint64_t view, const View& image) {
    std::scoped_lock lk{detail::mutex};
    if (detail::num_pending < detail::pending.size()) {
        detail::pending[detail::num_pending++] = {view, image};
    }
}

/// A pass begins (Scheduler::BeginRendering, after the census's NoteBegin). `State` is
/// Vulkan::RenderState (a template, so this header needs no Vulkan headers).
template <typename State>
inline void NoteBegin(const State& state, bool resumed) {
    std::scoped_lock lk{detail::mutex};
    using namespace detail;
    open = true;
    open_traced = active;
    if (!active) {
        num_pending = 0;
        return;
    }
    std::array<std::uint64_t, 9> targets{};
    for (std::uint32_t i = 0; i < state.num_color_attachments; ++i) {
        targets[i] = std::bit_cast<std::uint64_t>(state.color_attachments[i].image_view);
    }
    const auto& db = state.depth_stencil_attachment;
    targets[8] = (db.has_depth || db.has_stencil) ? std::bit_cast<std::uint64_t>(db.image_view) : 0;
    const bool same_targets =
        targets == last_targets && state.width == last_width && state.height == last_height;
    last_targets = targets;
    last_width = state.width;
    last_height = state.height;

    draws = 0;
    ++frame_passes;
    Append("{\"e\":\"begin\",\"frame\":%llu,\"pass\":%llu,\"width\":%u,\"height\":%u,"
           "\"layers\":%u,\"resumed\":%d,\"same_targets\":%d,\"attachments\":[",
           static_cast<unsigned long long>(frame_ends), static_cast<unsigned long long>(pass),
           std::uint32_t(state.width), std::uint32_t(state.height),
           std::uint32_t(state.num_layers), resumed ? 1 : 0, same_targets ? 1 : 0);
    bool first = true;
    static constexpr const char* color_slots[] = {"c0", "c1", "c2", "c3", "c4", "c5", "c6", "c7"};
    for (std::uint32_t i = 0; i < state.num_color_attachments && i < 8; ++i) {
        AppendAttachment(first, color_slots[i], targets[i],
                         std::uint32_t(state.color_attachments[i].image_layout),
                         state.color_attachments[i].is_clear != 0);
    }
    if (db.has_depth) {
        AppendAttachment(first, "depth", targets[8], std::uint32_t(db.image_layout),
                         db.depth_clear);
    }
    if (db.has_stencil) {
        AppendAttachment(first, "stencil", targets[8], std::uint32_t(db.image_layout),
                         db.stencil_clear);
    }
    lines += "]}\n";
    num_pending = 0;
}

/// A draw is recorded in the open pass.
inline void NoteDraw() {
    std::scoped_lock lk{detail::mutex};
    if (detail::active && detail::open_traced) {
        ++detail::draws;
        ++detail::frame_draws;
    }
}

/// The open pass ends (Scheduler::EndRendering): the census's cause and attributed ender (the
/// EndRendering caller for Other, the FlushBarriers caller for Barrier).
inline void NoteEnd(BbPassCensus::Why why, const void* ender) {
    std::scoped_lock lk{detail::mutex};
    using namespace detail;
    const bool was_open = std::exchange(open, false);
    if (!active || !was_open) {
        return;
    }
    if (!open_traced) {
        // Began before the trace: its attachments and earlier draws are unknown.
        Append("{\"e\":\"end\",\"frame\":%llu,\"partial\":1,\"why\":\"%s\"}\n",
               static_cast<unsigned long long>(frame_ends),
               BbPassCensus::why_names[std::size_t(why)]);
        return;
    }
    Append("{\"e\":\"end\",\"frame\":%llu,\"pass\":%llu,\"why\":\"%s\",\"ender\":\"%s\","
           "\"draws\":%llu}\n",
           static_cast<unsigned long long>(frame_ends), static_cast<unsigned long long>(pass),
           BbPassCensus::why_names[std::size_t(why)],
           ender ? BbPassCensus::detail::Offset(ender).c_str() : "",
           static_cast<unsigned long long>(draws));
    ++pass;
}

/// An access requested through Runtime::Transit (before the barrier flush that may end the open
/// pass): `whole` when it covers every level and layer.
inline void NoteUse(std::uint64_t uid, std::uint32_t layout, std::uint64_t access,
                    std::uint64_t stage, bool whole, bool in_pass) {
    std::scoped_lock lk{detail::mutex};
    if (!detail::active) {
        return;
    }
    using namespace detail;
    ++frame_uses;
    Append("{\"e\":\"use\",\"frame\":%llu,\"pass\":%llu,\"requested_in_pass\":%d,\"uid\":%llu,"
           "\"layout\":%u,\"access\":%llu,\"stage\":%llu,\"whole\":%d}\n",
           static_cast<unsigned long long>(frame_ends), static_cast<unsigned long long>(pass),
           in_pass && open && open_traced ? 1 : 0, static_cast<unsigned long long>(uid), layout,
           static_cast<unsigned long long>(access), static_cast<unsigned long long>(stage),
           whole ? 1 : 0);
}

/// On stage A at each frame end (with the census): starts, writes and ends the trace.
inline void NoteFrameEnd() {
    const auto& config = detail::Get();
    std::scoped_lock lk{detail::mutex};
    using namespace detail;
    if (active) {
        Append("{\"e\":\"frame\",\"frame\":%llu,\"passes\":%llu,\"draws\":%llu,\"uses\":%llu,"
               "\"open_at_end\":%d}\n",
               static_cast<unsigned long long>(frame_ends),
               static_cast<unsigned long long>(frame_passes),
               static_cast<unsigned long long>(frame_draws),
               static_cast<unsigned long long>(frame_uses), open ? 1 : 0);
        if (std::fwrite(lines.data(), 1, lines.size(), file) != lines.size()) {
            std::fprintf(stderr, "STOP: pass trace: writing %s failed\n", config.out);
            std::abort();
        }
        lines.clear();
        frame_passes = frame_draws = frame_uses = 0;
    }
    ++frame_ends;
    if (frame_ends == config.first) {
        file = std::fopen(config.out, "w");
        if (!file) {
            std::fprintf(stderr, "STOP: pass trace: cannot create %s\n", config.out);
            std::abort();
        }
        active = true;
        Append("{\"e\":\"start\",\"first\":%llu,\"count\":%llu,\"open_at_start\":%d}\n",
               static_cast<unsigned long long>(config.first),
               static_cast<unsigned long long>(config.count), open ? 1 : 0);
    } else if (active && frame_ends == config.first + config.count) {
        active = false;
        const bool flushed = std::fflush(file) == 0;
        if (std::fclose(file) != 0 || !flushed) {
            std::fprintf(stderr, "STOP: pass trace: writing %s failed at close\n", config.out);
            std::abort();
        }
        file = nullptr;
        std::printf("Pass trace: frames %llu..%llu written to %s\n",
                    static_cast<unsigned long long>(config.first),
                    static_cast<unsigned long long>(config.first + config.count - 1), config.out);
        std::fflush(stdout);
    }
}

} // namespace BbPassTrace
#endif
