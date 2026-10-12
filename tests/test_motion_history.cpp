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
    // As the rasterizer: every draw of the mesh is noted, then the eligible ones prepared.
    const History::GateKey mesh_of_draw{.shader = draw.shader, .geometry = draw.geometry,
                                        .indices = draw.indices, .index_count = draw.index_count,
                                        .instances = draw.instances,
                                        .first_instance = draw.first_instance};
    const auto drawn = [&](History& h, uint64_t palette_address, bool eligible = true) {
        h.NoteDraw(mesh_of_draw, palette_address);
        return eligible ? h.Prepare(draw) : History::Allocation{};
    };
    History history(32);
    history.NextFrame();
    const auto first = drawn(history, 0xA);
    const auto duplicate = drawn(history, 0xB);
    assert(first.store && !first.load && first.first_vertex == 100);
    assert(first.first_instance == 100 && first.instances == 2);
    assert(duplicate.store == first.store + 16); // shared geometry, different draw
    assert(history.Used() == 32); // firstInstance must NOT multiply the allocation
    assert(!drawn(history, 0xC).store && history.stats.exhausted == 1);
    // The mesh had three copies: from now on it never loads history (no instance identity:
    // culling reorders copies, and loading by order gave other copies' positions).
    history.NextFrame();
    assert(history.MultiInstanceMeshes() == 1);
    assert(!drawn(history, 0xB).load && !drawn(history, 0xA).load);
    history.NextFrame();
    assert(!drawn(history, 0xA).load && history.stats.loaded == 0); // one copy now: still multi
    assert(history.stats.multi >= 4);

    // A single-instance mesh loads its history.
    History single(64);
    single.NextFrame();
    const auto alone = drawn(single, 0xA);
    single.NextFrame();
    assert(drawn(single, 0xA).load == alone.store && single.stats.loaded == 1);
    single.NextFrame(); // a missing frame invalidates the match
    single.NextFrame();
    assert(!drawn(single, 0xA).load);

    // Sol-2's controls, with the mesh known as multi-instance: no cross-copy load.
    const auto known_multi = [&](History& h) {
        h.NextFrame();
        drawn(h, 0xA);
        drawn(h, 0xB);
        h.NextFrame();
    };
    History k1(256); // previous A; current B (newly visible) first, then A
    known_multi(k1);
    drawn(k1, 0xA);
    k1.NextFrame();
    assert(!drawn(k1, 0xB).load && !drawn(k1, 0xA).load);
    History k2(256); // previous A; current B only
    known_multi(k2);
    drawn(k2, 0xA);
    k2.NextFrame();
    assert(!drawn(k2, 0xB).load);
    // Two visible copies, the moving gate passing A in one frame and B in the next: every
    // copy is noted, so the mesh is multi-instance from that first frame on.
    History k3(256);
    k3.NextFrame();
    drawn(k3, 0xA);              // A eligible (residual (a): first frame of multiplicity)
    drawn(k3, 0xB, false);       // B still: noted, not prepared
    k3.NextFrame();
    drawn(k3, 0xA, false);
    assert(!drawn(k3, 0xB).load && k3.stats.loaded == 0);
    // The documented residuals (not ruled out; stated in Prepare and docs/upscaler.md):
    // (a) the frame in which a mesh first shows a second copy, the copy drawn first loads;
    History r1(256);
    r1.NextFrame();
    const auto r1_a = drawn(r1, 0xA);
    r1.NextFrame();
    assert(drawn(r1, 0xB).load == r1_a.store); // B first: takes A's (residual (a))
    assert(!drawn(r1, 0xA).load);              // the second copy: refused
    // (b) a lone instance replaced by another one of the same mesh between two frames.
    History r2(256);
    r2.NextFrame();
    const auto r2_a = drawn(r2, 0xA);
    r2.NextFrame();
    assert(drawn(r2, 0xB).load == r2_a.store); // residual (b)

    // Provenance counters: palette address sets of multi-instance meshes, frame to frame.
    History prov(1024);
    prov.NextFrame();
    drawn(prov, 0xA, false);
    drawn(prov, 0xB, false);
    prov.NextFrame();
    drawn(prov, 0xA, false); // same set, same order
    drawn(prov, 0xB, false);
    prov.NextFrame();
    drawn(prov, 0xB, false); // same set, reordered
    drawn(prov, 0xA, false);
    prov.NextFrame();
    drawn(prov, 0xC, false); // changed
    drawn(prov, 0xA, false);
    prov.NextFrame();
    assert(prov.stats.addresses_same == 1 && prov.stats.addresses_reordered == 1 &&
           prov.stats.addresses_changed == 1);

    // A changed index buffer may reference formerly unused holes inside the same min/max.
    // Do not accept that history merely because its addresses/counts stayed the same.
    history.NextFrame();
    auto changed = draw;
    changed.topology++;
    assert(!history.Prepare(changed).load);
    changed = draw;
    changed.first_instance++;
    assert(!history.Prepare(changed).load);
    history.NextFrame();
    changed = draw;
    changed.vertices.first++;
    assert(!history.Prepare(changed).load);

    History overflow(32);
    draw.vertices.first = 0;
    draw.vertices.count = INT32_MAX;
    draw.instances = UINT32_MAX;
    draw.first_instance = 0;
    assert(!overflow.Prepare(draw).store);
    assert(overflow.stats.exhausted == 1);
    draw.first_instance = 10;
    assert(!overflow.Prepare(draw).store && overflow.stats.invalid == 1);
    draw.first_instance = 0;
    draw.vertices.first = INT32_MAX;
    draw.vertices.count = 2;
    assert(!overflow.Prepare(draw).store && overflow.stats.invalid == 2);

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
    std::puts("Motion history: PASS (ranges, restart, offsets, matching, multi-instance meshes, residuals, provenance, capacity, gating, index cache, jitter)");
}
