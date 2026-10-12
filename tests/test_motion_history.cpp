// SPDX-License-Identifier: GPL-2.0-or-later
// Standalone CPU regression checks: no game, Vulkan device or window required.
#include <array>
#include <cassert>
#include <cstdio>
#include "gpu/shadps4/video_core/renderer_vulkan/motion_history.h"

using namespace Vulkan::Motion;

int main() {
    const std::array<uint16_t, 5> indices{1000, 1007, 1002, 1007, UINT16_MAX};
    const auto range = IndexedRange(std::span(indices), -900, true);
    assert(range.first == 100 && range.count == 8);
    assert(IndexedRange(std::span(indices), -1001, true).count == 0);
    assert(IndexedRange(std::span(indices), 0, false).count == 64536);
    const std::array<uint32_t, 3> large{100000, 100003, UINT32_MAX};
    assert(IndexedRange(std::span(large), 7, true).count == 4);
    assert(IndexedRange(std::span(large), INT32_MAX, true).count == 0);
    const std::array<uint16_t, 1> restart{UINT16_MAX};
    assert(IndexedRange(std::span(restart), 0, true).count == 0);
    assert(IndexedRange(std::span<const uint16_t>{}, 0, false).count == 0);

    Draw draw{.shader = 1, .geometry = 2, .indices = 3, .topology = 4,
              .index_count = 5, .instances = 2, .first_instance = 100,
              .vertices = range};
    using Anchor = History::Anchor;
    const Anchor at_a{0, 0, 0}, at_b{10, 0, 0}, a_moved{0.3f, 0, 0};
    History history(32);
    history.NextFrame();
    const auto first = history.Prepare(draw, at_a);
    const auto duplicate = history.Prepare(draw, at_b);
    assert(first.store && !first.load && first.first_vertex == 100);
    assert(first.first_instance == 100 && first.instances == 2);
    assert(duplicate.store == first.store + 16); // shared geometry, different draw
    assert(history.Used() == 32); // firstInstance must NOT multiply the allocation
    assert(!history.Prepare(draw, at_a).store && history.stats.exhausted == 1);
    // Copies of one mesh are identified by their anchors, not by draw order (culling reorders
    // copies; loading by order gave another copy's positions, hundreds of pixels off).
    history.NextFrame();
    const auto b_first = history.Prepare(draw, at_b);
    const auto a_second = history.Prepare(draw, a_moved); // A moved 0.3, within MaxStep
    assert(b_first.load == duplicate.store && a_second.load == first.store);
    assert(history.stats.loaded == 2);

    // Sol-2's controls: none may load another copy's history.
    // 1. Previous: A only. Current: B (newly visible) first, then A.
    History c1(64);
    c1.NextFrame();
    const auto c1_a = c1.Prepare(draw, at_a);
    c1.NextFrame();
    assert(!c1.Prepare(draw, at_b).load && c1.stats.ambiguous == 1);
    assert(c1.Prepare(draw, at_a).load == c1_a.store);
    // 2. Previous: A only. Current: B only.
    History c2(64);
    c2.NextFrame();
    c2.Prepare(draw, at_a);
    c2.NextFrame();
    assert(!c2.Prepare(draw, at_b).load && c2.stats.ambiguous == 1);
    // 3. Two visible copies; the moving gate lets A through in one frame and B in the next
    //    (each frame reaches Prepare once with the same draw identity).
    History c3(64);
    c3.NextFrame();
    c3.Prepare(draw, at_a);
    c3.NextFrame();
    assert(!c3.Prepare(draw, at_b).load && c3.stats.ambiguous == 1);
    // Two previous copies near this one (within 2 * MaxStep): ambiguous.
    History close(64);
    close.NextFrame();
    close.Prepare(draw, Anchor{0, 0, 0});
    close.Prepare(draw, Anchor{0.6f, 0, 0});
    close.NextFrame();
    assert(!close.Prepare(draw, Anchor{0.3f, 0, 0}).load && close.stats.ambiguous == 1);
    // A previous copy is taken once: a second draw at the same place loads none.
    History twice(64);
    twice.NextFrame();
    const auto once = twice.Prepare(draw, at_a);
    twice.NextFrame();
    assert(twice.Prepare(draw, at_a).load == once.store);
    assert(!twice.Prepare(draw, at_a).load && twice.stats.ambiguous == 1);
    // No anchor (no palette): no identity, no history; nor for a draw whose previous copy had
    // none.
    History bare(64);
    bare.NextFrame();
    bare.Prepare(draw, std::nullopt);
    bare.NextFrame();
    assert(!bare.Prepare(draw, std::nullopt).load && bare.stats.no_identity == 1);
    assert(!bare.Prepare(draw, at_a).load && bare.stats.ambiguous == 1);
    // A non-finite anchor is never nearby.
    History nan(64);
    nan.NextFrame();
    nan.Prepare(draw, Anchor{NAN, 0, 0});
    nan.NextFrame();
    assert(!nan.Prepare(draw, Anchor{NAN, 0, 0}).load);
    // More copies than tracked: none of them loads.
    History many(64 * (History::MaxCopies + 1));
    many.NextFrame();
    for (uint32_t i = 0; i <= History::MaxCopies; ++i) {
        many.Prepare(draw, Anchor{float(i) * 10, 0, 0});
    }
    many.NextFrame();
    assert(!many.Prepare(draw, at_a).load);

    history.NextFrame(); // a missing frame invalidates the match
    history.NextFrame();
    assert(!history.Prepare(draw, at_a).load);

    // A changed index buffer may reference formerly unused holes inside the same min/max.
    // Do not accept that history merely because its addresses/counts stayed the same.
    history.NextFrame();
    auto changed = draw;
    changed.topology++;
    assert(!history.Prepare(changed, at_a).load);
    changed = draw;
    changed.first_instance++;
    assert(!history.Prepare(changed, at_a).load);
    history.NextFrame();
    changed = draw;
    changed.vertices.first++;
    assert(!history.Prepare(changed, at_a).load);

    History overflow(32);
    draw.vertices.first = 0;
    draw.vertices.count = INT32_MAX;
    draw.instances = UINT32_MAX;
    draw.first_instance = 0;
    assert(!overflow.Prepare(draw, std::nullopt).store);
    assert(overflow.stats.exhausted == 1);
    draw.first_instance = 10;
    assert(!overflow.Prepare(draw, std::nullopt).store && overflow.stats.invalid == 1);
    draw.first_instance = 0;
    draw.vertices.first = INT32_MAX;
    draw.vertices.count = 2;
    assert(!overflow.Prepare(draw, std::nullopt).store && overflow.stats.invalid == 2);

    // Small skeletons: history only while the bone palette changes (weapon spinning).
    History gated(1024);
    const History::GateKey weapon{.shader = 7, .index_count = 3, .instances = 1};
    const auto frame = [&](uint64_t palette) {
        gated.NextFrame();
        return gated.Moving(weapon, palette);
    };
    assert(!gated.Moving(weapon, 1) && gated.stats.still == 1); // first sight
    assert(!frame(1));                    // same bones: camera motion
    assert(frame(2) && frame(3));         // spinning
    assert(!frame(3));                    // stopped
    gated.NextFrame();
    gated.NextFrame();
    assert(!gated.Moving(weapon, 4));     // after a skipped frame: no comparison
    assert(!gated.Moving(weapon, 5));     // second copy of the same draw, same frame
    // Static copies drawn in another order (culling): neither is moving.
    assert(!frame(5) && !gated.Moving(weapon, 4));
    // One copy's palette changed: that copy is moving, the other is still.
    assert(frame(6) && !gated.Moving(weapon, 5));
    // The mesh not drawn last frame: no comparison.
    gated.NextFrame();
    gated.NextFrame();
    assert(!gated.Moving(weapon, 7));
    assert(ClassifyBuffer(864) == BufferRole::Other && ClassifyBuffer(416) == BufferRole::Other);
    assert(ClassifyBuffer(96) == BufferRole::SmallSkeleton &&
           ClassifyBuffer(384) == BufferRole::SmallSkeleton);
    assert(ClassifyBuffer(656) == BufferRole::Skeleton && ClassifyBuffer(64) == BufferRole::Other);

    // Index ranges: reused between full scans, rescanned every Revalidate frames, trimmed.
    IndexRangeCache ranges;
    uint32_t scans = 0;
    const IndexRangeCache::Key mesh{.address = 0x1000, .count = 3, .index_size = 2};
    const auto scan = [&] {
        ++scans;
        return IndexRangeCache::Result{{4, 3}, 99};
    };
    assert(ranges.Get(mesh, 0, scan).topology == 99 && scans == 1);
    assert(ranges.Get(mesh, IndexRangeCache::Revalidate - 1, scan).range == (VertexRange{4, 3}));
    assert(scans == 1);
    ranges.Get(mesh, IndexRangeCache::Revalidate, scan);
    assert(scans == 2);
    ranges.Trim(IndexRangeCache::Revalidate + IndexRangeCache::Unused + 1);
    assert(ranges.Size() == 0);

    assert(JitterPhases(1920, 1920) == 8);
    assert(JitterPhases(1280, 1920) == 18);
    assert(JitterPhases(960, 1920) == 32);
    assert(JitterPhases(640, 1920) == 72);
    assert(JitterPhases(0, 1920) == 8);
    std::puts("Motion history: PASS (ranges, restart, offsets, matching, copies, capacity, gating, index cache, jitter)");
}
