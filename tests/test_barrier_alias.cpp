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
#include "video_core/buffer_cache/buffer.h"
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

    write(arena_b, 0, 0);
    Expect("an empty range accesses nothing", !runtime.IsBufferAccessed(&arena_a, Page, 256) &&
                                                  !runtime.IsBufferAccessed(&arena_a, Page, 0));
    runtime.FlushBarriers();
    scheduler.Finish();

    munmap(map, 4 * Page);
    std::printf("%s: %u failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
