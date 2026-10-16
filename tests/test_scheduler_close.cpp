// SPDX-License-Identifier: GPL-2.0-or-later
// Scheduler::Close (docs/macos/moltenvk-prefill.md): with MoltenVK's prefill, a command buffer
// reset or destroyed unsubmitted still has its Metal command buffer committed, outside the
// scheduler's ticks. Close submits the open buffer through the normal submission and waits:
// - an unsubmitted recorded tail (a fill, with BB_PERF's timestamps around it) lands, with the
//   threaded recorder and with recording on the caller's thread, and so do the commands the
//   submit callback records into that final submission (the rasterizer's pending barriers);
// - Flush and Finish after Close return at once; a destroyed closed scheduler opens nothing,
//   nor does an unclosed one with nothing recorded (BB_PERF timestamps attached);
// - stops, exit 23 with the scheduler's message, in child processes forked before this process
//   touches Vulkan: a scheduler with an unsubmitted recorded command destroyed without Close,
//   and recording after Close.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include <vk_mem_alloc.h>

namespace {
unsigned failures = 0;
void Expect(const char* what, bool ok) {
    std::printf("%-78s %s\n", what, ok ? "ok" : "FAIL");
    failures += ok ? 0 : 1;
}

struct HostBuffer {
    VmaAllocator allocator;
    VkBuffer buffer{};
    VmaAllocation allocation{};
    u32* words{};
    HostBuffer(VmaAllocator allocator_, u64 bytes) : allocator{allocator_} {
        const VkBufferCreateInfo info{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                      .size = bytes,
                                      .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        const VmaAllocationCreateInfo create{
            .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                     VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
            .usage = VMA_MEMORY_USAGE_AUTO,
            .requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT};
        VmaAllocationInfo allocated{};
        const VkResult result =
            vmaCreateBuffer(allocator, &info, &create, &buffer, &allocation, &allocated);
        if (result != VK_SUCCESS) {
            std::fprintf(stderr, "host buffer: %d\n", result);
            std::exit(2);
        }
        words = static_cast<u32*>(allocated.pMappedData);
    }
    ~HostBuffer() {
        vmaDestroyBuffer(allocator, buffer, allocation);
    }
};

struct Dispatch {
    vk::detail::DynamicLoader loader;
    vk::detail::DispatchLoaderDynamic d;
    explicit Dispatch(const Vulkan::Instance& instance) {
        d.init(loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr"));
        d.init(instance.GetInstance());
        d.init(instance.GetDevice());
    }
};

constexpr u32 Words = 4096;

/// Records a fill of `buffer` with `value` (never submitted by the caller).
void RecordFill(Vulkan::Scheduler& scheduler, const Dispatch& dispatch, VkBuffer buffer,
                u32 value) {
    const auto* d = &dispatch.d;
    scheduler.Record([buffer, value, d](vk::CommandBuffer cmd) {
        cmd.fillBuffer(vk::Buffer{buffer}, 0, Words * 4, value, *d);
        const vk::MemoryBarrier2 to_host{.srcStageMask = vk::PipelineStageFlagBits2::eAllTransfer,
                                         .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                                         .dstStageMask = vk::PipelineStageFlagBits2::eHost,
                                         .dstAccessMask = vk::AccessFlagBits2::eHostRead};
        cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &to_host}, *d);
    });
}

bool AllWords(const u32* words, u32 value) {
    for (u32 i = 0; i < Words; ++i) {
        if (words[i] != value) {
            return false;
        }
    }
    return true;
}

/// Child: an unsubmitted recorded fill, the scheduler destroyed without Close.
[[noreturn]] void DestroyWithoutClose() {
    Vulkan::Instance instance(0, false);
    Dispatch dispatch{instance};
    HostBuffer host{instance.GetAllocator(), Words * 4};
    {
        Vulkan::Scheduler scheduler(instance, true);
        RecordFill(scheduler, dispatch, host.buffer, 0x5a5a5a5a);
    }
    std::_Exit(0); // not reached when the destructor stops
}

/// Child: a command recorded after Close.
[[noreturn]] void RecordAfterClose() {
    Vulkan::Instance instance(0, false);
    Dispatch dispatch{instance};
    HostBuffer host{instance.GetAllocator(), Words * 4};
    Vulkan::Scheduler scheduler(instance, true);
    scheduler.Close();
    RecordFill(scheduler, dispatch, host.buffer, 0x5a5a5a5a);
    std::_Exit(0); // not reached when Record stops
}

struct ChildResult {
    bool forked = false, waited = false;
    int status = 0;
    std::string stderr_text;
};

/// Runs `body` in a forked child with its stderr captured.
template <typename Body>
ChildResult RunChild(Body body, const char* perf_name, const auto& perf_out) {
    ChildResult result;
    int pipe_fds[2];
    if (pipe(pipe_fds) != 0) {
        return result;
    }
    std::fflush(stdout);
    std::fflush(stderr);
    const pid_t child = fork();
    if (child < 0) {
        return result;
    }
    if (child == 0) {
        dup2(pipe_fds[1], STDERR_FILENO);
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        perf_out(perf_name);
        body();
    }
    result.forked = true;
    close(pipe_fds[1]);
    char chunk[4096];
    for (ssize_t n; (n = read(pipe_fds[0], chunk, sizeof(chunk))) > 0;) {
        result.stderr_text.append(chunk, size_t(n));
    }
    close(pipe_fds[0]);
    result.waited = waitpid(child, &result.status, 0) == child;
    std::remove(("out/scheduler-close-test-" + std::to_string(child) + "-" + perf_name +
                 ".jsonl")
                    .c_str());
    return result;
}

bool StoppedWith(const ChildResult& r, const char* message) {
    return r.forked && r.waited && WIFEXITED(r.status) && WEXITSTATUS(r.status) == 23 &&
           r.stderr_text.find(message) != std::string::npos;
}
} // namespace

int main() {
    // BB_PERF before anything reads it: the closed buffer carries its timestamps. Each process
    // writes its own report (BB_PERF_OUT must be a new file), removed at the end.
    setenv("BB_PERF", "1", 1);
    const auto perf_out = [](const char* who) {
        const std::string path = "out/scheduler-close-test-" + std::to_string(getpid()) + "-" +
                                 who + ".jsonl";
        std::remove(path.c_str());
        setenv("BB_PERF_OUT", path.c_str(), 1);
        return path;
    };

    const auto destroyed = RunChild(DestroyWithoutClose, "destroy", perf_out);
    Expect("child forked and reaped (destroy without Close)", destroyed.forked && destroyed.waited);
    Expect("destroyed with an unsubmitted recorded command, no Close: exit 23 with its message",
           StoppedWith(destroyed, "destroyed without Scheduler::Close"));
    const auto after = RunChild(RecordAfterClose, "after", perf_out);
    Expect("child forked and reaped (record after Close)", after.forked && after.waited);
    Expect("recording after Close: exit 23 with its message",
           StoppedWith(after, "recording after Scheduler::Close"));
    if (!StoppedWith(destroyed, "destroyed without") || !StoppedWith(after, "after Scheduler")) {
        std::printf("child stderr (destroy):\n%s\nchild stderr (after):\n%s\n",
                    destroyed.stderr_text.c_str(), after.stderr_text.c_str());
    }
    const std::string report = perf_out("parent");

    Vulkan::Instance instance(0, false);
    Dispatch dispatch{instance};
    HostBuffer host{instance.GetAllocator(), Words * 4};
    HostBuffer callback_host{instance.GetAllocator(), Words * 4};
    for (const bool threaded : {true, false}) {
        std::memset(host.words, 0, Words * 4);
        std::memset(callback_host.words, 0, Words * 4);
        const u32 value = threaded ? 0x11223344u : 0x55667788u;
        {
            Vulkan::Scheduler scheduler(instance, threaded);
            scheduler.InitPerf(false);
            // Like the rasterizer's pending barriers: the submit callback records into the
            // submission it precedes, Close's final one included.
            Vulkan::Scheduler* const self = &scheduler;
            const VkBuffer callback_buffer = callback_host.buffer;
            const Dispatch* const callback_dispatch = &dispatch;
            scheduler.SetSubmitCallback([self, callback_buffer, callback_dispatch,
                                         value](Vulkan::SubmitInfo&) {
                RecordFill(*self, *callback_dispatch, callback_buffer, ~value);
            });
            RecordFill(scheduler, dispatch, host.buffer, value);
            Expect(threaded ? "threaded recorder: the tail is not executed before Close"
                            : "caller-thread recording: the tail is not executed before Close",
                   AllWords(host.words, 0));
            scheduler.Close();
            Expect(threaded ? "threaded recorder: Close submitted the tail and waited"
                            : "caller-thread recording: Close submitted the tail and waited",
                   AllWords(host.words, value));
            Expect("  ... the submit callback's commands are in that final submission",
                   AllWords(callback_host.words, ~value));
            scheduler.Flush();
            scheduler.Finish();
            scheduler.Close();
            Expect("  ... Flush, Finish and a second Close after Close return", true);
        }
        Expect("  ... a closed scheduler is destroyed (nothing open)", true);
    }
    {
        Vulkan::Scheduler scheduler(instance, true);
        scheduler.InitPerf(false);
        scheduler.Finish();
    }
    Expect("an unclosed scheduler with nothing recorded (BB_PERF timestamps) is destroyed",
           true);

    std::remove(report.c_str());
    std::printf("scheduler-close-test: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
