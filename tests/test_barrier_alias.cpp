// SPDX-License-Identifier: GPL-2.0-or-later
// The barrier tracker keys guest memory by guest address (Apple, vk_runtime.cpp TrackedRange):
// an access through one imported arena is found when the same guest bytes are checked through
// another arena, so the command gets its barrier. Keyed by Buffer*, it was missed. Detecting the
// dependency is what this tests; the GPU does not keep two imports of the same bytes coherent
// within a submission, which the buffer cache avoids (HostImportArenaMap::Obtain).
#include <sys/mman.h>
#include <cassert>
#include <cstdio>
#include <string>
#include <utility>
#include "video_core/buffer_cache/buffer.h"
#include "bbport_toggles.h"
#include "video_core/renderer_vulkan/vk_gs_emulation.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/blit_helper.h" // Runtime destroys its BlitHelper

using VideoCore::Buffer;
using VideoCore::MemoryType;

namespace {
unsigned failures = 0;
void Expect(const std::string& what, bool ok) {
    std::printf("%-86s %s\n", what.c_str(), ok ? "ok" : "FAIL");
    failures += ok ? 0 : 1;
}
} // namespace

int main() {
    Vulkan::Instance instance(0, false);
    Vulkan::Scheduler scheduler(instance, true);
    Vulkan::Runtime runtime(instance, scheduler);

    // Two imports overlapping by one page: guest + Page is arena a at Page and arena b at 0.
    constexpr u64 Page = 64 * 1024;
    void* map = mmap(nullptr, 4 * Page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(map != MAP_FAILED);
    const VAddr guest = (reinterpret_cast<VAddr>(map) + Page - 1) & ~(Page - 1);
    Buffer arena_a(instance, guest, 2 * Page, MemoryType::ImportedHost, "alias arena a");
    Buffer arena_b(instance, guest + Page, 2 * Page, MemoryType::ImportedHost, "alias arena b");
    Buffer y(instance, 0, Page, MemoryType::HostCached, "alias private");

    const auto write = [&](const Buffer& buffer, u64 offset, u64 size) {
        runtime.AccessBuffer(&buffer, offset, size, vk::PipelineStageFlagBits2::eCopy,
                             vk::AccessFlagBits2::eTransferWrite);
    };
    const auto read = [&](const Buffer& buffer, u64 offset, u64 size) {
        runtime.AccessBuffer(&buffer, offset, size, vk::PipelineStageFlagBits2::eCopy,
                             vk::AccessFlagBits2::eTransferRead);
    };

    write(arena_a, Page, 256);
    Expect("write through arena a is found through arena b (same guest bytes)",
           runtime.IsBufferAccessed(&arena_b, 0, 256));
    Expect("... and through arena a itself", runtime.IsBufferAccessed(&arena_a, Page, 256));
    Expect("disjoint guest bytes through arena b: not accessed",
           !runtime.IsBufferAccessed(&arena_b, Page, 256));
    Expect("a private buffer at the same offsets: not accessed",
           !runtime.IsBufferAccessed(&y, 0, 256));
    runtime.FlushBarriers();
    Expect("after the barrier: nothing pending", !runtime.IsBufferAccessed(&arena_b, 0, 256));

    read(arena_b, 0, 256);
    Expect("a read through arena b is found by a write check through arena a (WAR)",
           runtime.IsBufferAccessed(&arena_a, Page, 256, true));
    Expect("... but not by a read-only check (reads do not conflict)",
           !runtime.IsBufferAccessed(&arena_a, Page, 256, false));
    runtime.FlushBarriers();

    // Fixed-function input reads recorded after a draw (Rasterizer::InputRead): an indirect
    // argument read is tracked, so a later write to the arguments gets its barrier.
    runtime.AccessBuffer(&y, 64, 16, vk::PipelineStageFlagBits2::eDrawIndirect,
                         vk::AccessFlagBits2::eIndirectCommandRead);
    Expect("an indirect argument read is found by a later write check (WAR)",
           runtime.IsBufferAccessed(&y, 64, 16, true));
    runtime.FlushBarriers();
    runtime.AccessBuffer(&y, 128, 16, vk::PipelineStageFlagBits2::eVertexAttributeInput,
                         vk::AccessFlagBits2::eVertexAttributeRead);
    runtime.AccessBuffer(&y, 256, 16, vk::PipelineStageFlagBits2::eIndexInput,
                         vk::AccessFlagBits2::eIndexRead);
    Expect("vertex and index reads are found by later write checks (WAR)",
           runtime.IsBufferAccessed(&y, 128, 16, true) &&
               runtime.IsBufferAccessed(&y, 256, 16, true));
    runtime.FlushBarriers();

    // The GS ES kernel's guest index reads are whole dwords (Vulkan::GsIndexLoadRange). With
    // 4 KiB read rounding off, the tracked range must hold the halfword before a u16 range that
    // starts mid-dword and the one after a range that ends mid-dword.
    {
        using Vulkan::GsIndexLoadRange;
        Expect("ES index load range: one u16 at offset 2 loads [0, 4)",
               GsIndexLoadRange(2, 2) == std::pair<u64, u64>{0, 4});
        Expect("ES index load range: three u16 at offset 4 load [4, 12)",
               GsIndexLoadRange(4, 6) == std::pair<u64, u64>{4, 12});
        Expect("ES index load range: dword-aligned u32 indices load exactly their bytes",
               GsIndexLoadRange(8, 8) == std::pair<u64, u64>{8, 16});
        __atomic_or_fetch(&runtime_disabled_optimizations, BbToggle::CoarseReadTracking,
                          __ATOMIC_RELAXED);
        const auto record = [&](u64 offset, u64 bytes) {
            const auto [begin, end] = GsIndexLoadRange(offset, bytes);
            runtime.AccessBuffer(&y, begin, end - begin, vk::PipelineStageFlagBits2::eComputeShader,
                                 vk::AccessFlagBits2::eShaderRead);
        };
        record(2, 2);
        Expect("leading halfword: a write to [0, 2) is found (exact tracking)",
               runtime.IsBufferAccessed(&y, 0, 2, true));
        runtime.FlushBarriers();
        record(4, 6);
        Expect("trailing halfword: a write to [10, 12) is found (exact tracking)",
               runtime.IsBufferAccessed(&y, 10, 2, true));
        Expect("... and the dword after the load is not (the tracking is exact)",
               !runtime.IsBufferAccessed(&y, 12, 4, true));
        runtime.FlushBarriers();
        runtime.AccessBuffer(&y, 2, 2, vk::PipelineStageFlagBits2::eComputeShader,
                             vk::AccessFlagBits2::eShaderRead);
        Expect("control: the index bytes alone ([2, 4)) miss the leading halfword",
               !runtime.IsBufferAccessed(&y, 0, 2, true));
        runtime.FlushBarriers();
        __atomic_and_fetch(&runtime_disabled_optimizations, ~BbToggle::CoarseReadTracking,
                           __ATOMIC_RELAXED);
    }

    write(arena_b, 0, 0);
    Expect("an empty range accesses nothing", !runtime.IsBufferAccessed(&arena_a, Page, 256) &&
                                                  !runtime.IsBufferAccessed(&arena_a, Page, 0));
    runtime.FlushBarriers();
    scheduler.Finish();

    munmap(map, 4 * Page);
    std::printf("%s: %u failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
