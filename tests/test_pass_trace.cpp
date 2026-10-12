// Pass trace (gpu/shim/bbport_pass_trace.h): a known sequence of passes, draws and image accesses
// over two traced frames, written to BB_PASS_TRACE_OUT for tests/test_pass_trace.sh, which checks
// the file and tools/macos/pass_trace.py's report of it. Run with BB_PASS_TRACE=1+2.
#include <cstdint>
#include <cstdio>
#include "bbport_pass_trace.h"

namespace {
struct FakeView {
    std::uint64_t handle;
};
struct Color {
    FakeView image_view{};
    std::uint32_t image_layout = 0;
    std::uint32_t is_clear = 0;
};
struct DepthStencil {
    FakeView image_view{};
    std::uint32_t image_layout = 0;
    bool has_depth = false, has_stencil = false, depth_clear = false, stencil_clear = false;
};
struct State {
    std::uint32_t num_color_attachments = 0;
    Color color_attachments[8];
    DepthStencil depth_stencil_attachment;
    std::uint32_t width = 100, height = 100, num_layers = 1;
};

constexpr std::uint64_t ColorView = 0x1000, DepthView = 0x2000;
constexpr std::uint32_t ColorAttachmentOptimal = 2, DepthStencilAttachmentOptimal = 3;
constexpr std::uint32_t DepthReadOnlyOptimal = 1000241001;
constexpr std::uint64_t SampledRead = 0x100000000ull, TransferWrite = 0x1000;
constexpr std::uint64_t StageClear = 0x800000000ull, StageFragment = 0x80;

State ColorDepth(bool color_clear, bool depth_clear) {
    State s;
    s.num_color_attachments = 1;
    s.color_attachments[0] = {{ColorView}, ColorAttachmentOptimal, color_clear ? 1u : 0u};
    auto& d = s.depth_stencil_attachment;
    d = {{DepthView}, DepthStencilAttachmentOptimal, true, true, depth_clear, false};
    return s;
}

void Pass(const State& s, bool resumed, int draws, BbPassCensus::Why why, const void* ender) {
    BbPassTrace::NoteBegin(s, resumed);
    for (int i = 0; i < draws; ++i) {
        BbPassTrace::NoteDraw();
    }
    BbPassTrace::NoteEnd(why, ender);
}

void Ender() {}

// The rasterizer notes the next pass's own guest-image views before it begins.
void NoteViews(bool color, bool depth) {
    BbPassTrace::ClearViews();
    if (color) {
        BbPassTrace::NoteView(ColorView, {.uid = 1, .format = 37, .texel_bytes = 4, .width = 100,
                                          .height = 100, .layers = 1, .level = 0});
    }
    if (depth) {
        BbPassTrace::NoteView(DepthView, {.uid = 2, .format = 130, .texel_bytes = 5,
                                          .width = 100, .height = 100, .layers = 1, .level = 0});
    }
}
} // namespace

int main() {
    if (!BbPassTrace::Enabled()) {
        std::fprintf(stderr, "FAIL: BB_PASS_TRACE is not set\n");
        return 1;
    }
    using Why = BbPassCensus::Why;
    const void* ender = reinterpret_cast<const void*>(&Ender);
    // Before the trace: nothing is recorded. The second pass is still open when it starts.
    NoteViews(true, true);
    Pass(ColorDepth(false, false), false, 5, Why::TargetChange, nullptr);
    BbPassTrace::NoteUse(1, 0, SampledRead, StageFragment, true, false);
    NoteViews(true, true);
    BbPassTrace::NoteBegin(ColorDepth(false, false), false);
    BbPassTrace::NoteDraw();
    BbPassTrace::NoteFrameEnd(); // frame end 1: the trace starts
    BbPassTrace::NoteDraw();     // not counted: its pass began before the trace
    BbPassTrace::NoteEnd(Why::Other, ender); // a partial end record

    // Frame 1. Pass 0: color loaded, depth cleared, 3 draws, ended by a barrier.
    NoteViews(true, true);
    Pass(ColorDepth(false, true), false, 3, Why::Barrier, ender);
    BbPassTrace::NoteUse(1, 5, SampledRead, StageFragment, true, false); // pass 0's color: read
    // Pass 1 resumes it (same state as recorded by the scheduler): loads color and depth.
    NoteViews(true, true);
    Pass(ColorDepth(false, false), true, 1, Why::Other, ender);
    BbPassTrace::NoteUse(2, 7, TransferWrite, StageClear, true, false); // pass 1's depth: dead
    // Pass 2: read-only depth, no draw (empty). Its color view was not noted for it (a proxy, a
    // redirect, or a stale handle noted for an earlier pass): unknown.
    State ro;
    ro.num_color_attachments = 1;
    ro.color_attachments[0] = {{ColorView}, ColorAttachmentOptimal, 0};
    NoteViews(false, true);
    ro.depth_stencil_attachment = {{DepthView}, DepthReadOnlyOptimal, true, false, false, false};
    Pass(ro, false, 0, Why::TargetChange, nullptr);
    BbPassTrace::NoteFrameEnd(); // frame end 2

    // Frame 2. Pass 3: clears the color (pass 1's color store is dead), no draw.
    State clear;
    clear.num_color_attachments = 1;
    clear.color_attachments[0] = {{ColorView}, ColorAttachmentOptimal, 1};
    NoteViews(true, false);
    Pass(clear, false, 0, Why::Submit, nullptr);
    // Pass 4: a helper pass (no views noted for it) reusing pass 3's view handle: unknown.
    State helper;
    helper.num_color_attachments = 1;
    helper.color_attachments[0] = {{ColorView}, ColorAttachmentOptimal, 0};
    Pass(helper, false, 0, Why::TargetChange, nullptr);
    BbPassTrace::NoteFrameEnd(); // frame end 3: the trace ends

    // After the trace: nothing is recorded.
    NoteViews(true, true);
    Pass(ColorDepth(false, false), false, 2, Why::TargetChange, nullptr);
    BbPassTrace::NoteFrameEnd();
    std::printf("pass-trace driver: done\n");
    return 0;
}
