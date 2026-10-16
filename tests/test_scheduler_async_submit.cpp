// SPDX-License-Identifier: GPL-2.0-or-later
// Asynchronous submission (docs/macos/async-submission.md): with threaded recording, Flush
// enqueues a submit packet and the recording thread ends, submits and begins. Device controls:
// - order: 300 consecutive small submissions (several share a record chunk), each a distinct
//   fill of its own slot, with increasing values; all land; Wait on a value still queued;
// - direct recording between asynchronous submissions;
// - the packet cap: with the recorder held, Flush MaxQueuedSubmits + 1 waits until it is released;
// - a delayed producer on another scheduler: a submission waiting on this scheduler's value is
//   not handed to the queue before that value is (its fill lands after the producer's);
// - the host-copy prefix: a submission waits for an earlier held copy and not for a later one;
// - Close with queued submissions submits them all;
// - the synchronous fallback (ThreadedRecording toggled off) with a submission still queued:
//   Finish drains it before taking the submit lock, no deadlock;
// - waits added through WaitBeforeNextSubmit are ordered like SubmitInfo waits (delayed
//   producer) and checked like them (self-wait stop);
// - the command-buffer pool cap: with the GPU held on an external semaphore, the pool stops
//   growing at CommandPool::MaxBuffers and resumes when the GPU retires;
// - stops (forked children, exit 23 with the message): a wait on a value not assigned yet,
//   a submission waiting on its own scheduler's open value, and a hung copy (one that waits on
//   its own submission: no real copy does, the 10 s hang detector), a self-wait through
//   WaitBeforeNextSubmit, a Wait on the recording thread itself, and an asynchronous scheduler
//   waiting on another one.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>
#include "bbport_copy.h"
#include "bbport_toggles.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_resource_pool.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include <vk_mem_alloc.h>

using namespace std::chrono_literals;

namespace {
unsigned failures = 0;
void Expect(const std::string& what, bool ok) {
    std::printf("%-84s %s\n", what.c_str(), ok ? "ok" : "FAIL");
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
        if (vmaCreateBuffer(allocator, &info, &create, &buffer, &allocation, &allocated) !=
            VK_SUCCESS) {
            std::fprintf(stderr, "host buffer\n");
            std::exit(2);
        }
        words = static_cast<u32*>(allocated.pMappedData);
        std::memset(words, 0, bytes);
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

/// Records a fill of `words` words at word `slot` of `buffer` with `value`.
void RecordFill(Vulkan::Scheduler& scheduler, const Dispatch& dispatch, VkBuffer buffer, u32 slot,
                u32 words, u32 value) {
    const auto* d = &dispatch.d;
    scheduler.Record([buffer, slot, words, value, d](vk::CommandBuffer cmd) {
        cmd.fillBuffer(vk::Buffer{buffer}, u64(slot) * 4, u64(words) * 4, value, *d);
        const vk::MemoryBarrier2 to_host{.srcStageMask = vk::PipelineStageFlagBits2::eAllTransfer,
                                         .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                                         .dstStageMask = vk::PipelineStageFlagBits2::eHost,
                                         .dstAccessMask = vk::AccessFlagBits2::eHostRead};
        cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &to_host}, *d);
    });
}

/// Holds the recording thread in a recorded command until released.
void RecordHold(Vulkan::Scheduler& scheduler, std::atomic<bool>* release) {
    scheduler.Record([release](vk::CommandBuffer) {
        while (!release->load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    });
}

struct ChildResult {
    bool forked = false, waited = false;
    int status = 0;
    std::string stderr_text;
};

template <typename Body>
ChildResult RunChild(Body body) {
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
        body();
        std::_Exit(0); // not reached when the body stops
    }
    result.forked = true;
    close(pipe_fds[1]);
    char chunk[4096];
    for (ssize_t n; (n = read(pipe_fds[0], chunk, sizeof(chunk))) > 0;) {
        result.stderr_text.append(chunk, size_t(n));
    }
    close(pipe_fds[0]);
    result.waited = waitpid(child, &result.status, 0) == child;
    return result;
}

bool StoppedWith(const ChildResult& r, const char* message) {
    return r.forked && r.waited && WIFEXITED(r.status) && WEXITSTATUS(r.status) == 23 &&
           r.stderr_text.find(message) != std::string::npos;
}

void ExpectStop(const char* what, const ChildResult& r, const char* message) {
    Expect(std::string{what} + ": child forked and reaped", r.forked && r.waited);
    const bool ok = StoppedWith(r, message);
    Expect(std::string{what} + ": exit 23 with its message", ok);
    if (!ok) {
        std::printf("  child stderr:\n%s\n", r.stderr_text.c_str());
    }
}
} // namespace

int main() {
    // Stops, in children forked before this process touches Vulkan.
    ExpectStop("WaitQueueSubmitted for the open value", RunChild([] {
                   Vulkan::Instance instance(0, false);
                   Vulkan::Scheduler scheduler(instance, true);
                   scheduler.WaitQueueSubmitted(scheduler.CurrentTick());
               }),
               "not assigned yet");
    ExpectStop("a submission waiting on its own scheduler's open value", RunChild([] {
                   Vulkan::Instance instance(0, false);
                   Vulkan::Scheduler scheduler(instance, true);
                   Vulkan::SubmitInfo info{};
                   info.AddWait(scheduler.GetWorkSemaphore()->Handle(), scheduler.CurrentTick());
                   scheduler.Flush(info);
               }),
               "waits on its own scheduler's value");
    ExpectStop("a host copy waiting on its own submission", RunChild([] {
                   Vulkan::Instance instance(0, false);
                   Vulkan::Scheduler scheduler(instance, true);
                   const u64 own = scheduler.CurrentTick();
                   Vulkan::Scheduler* const self = &scheduler;
                   scheduler.RecordHostCopy([self, own] {
                       BbCopy::Async([self, own] { self->GetWorkSemaphore()->Wait(own); });
                   });
                   scheduler.Finish();
               }),
               "have not finished in 10 s");
    ExpectStop("a self-wait through WaitBeforeNextSubmit", RunChild([] {
                   Vulkan::Instance instance(0, false);
                   Vulkan::Scheduler scheduler(instance, true);
                   scheduler.WaitBeforeNextSubmit(scheduler.GetWorkSemaphore()->Handle(),
                                                  scheduler.CurrentTick());
                   scheduler.Flush();
               }),
               "waits on its own scheduler's value");
    ExpectStop("a Wait on the recording thread", RunChild([] {
                   Vulkan::Instance instance(0, false);
                   Vulkan::Scheduler scheduler(instance, true);
                   const u64 earlier = scheduler.Flush();
                   Vulkan::Scheduler* const self = &scheduler;
                   scheduler.Record([self, earlier](vk::CommandBuffer) { self->Wait(earlier); });
                   scheduler.Finish();
               }),
               "on the scheduler's own recording thread");
    ExpectStop("an asynchronous scheduler waiting on another", RunChild([] {
                   Vulkan::Instance instance(0, false);
                   Vulkan::Scheduler first(instance, true);
                   Vulkan::Scheduler second(instance, true);
                   const u64 value = first.Flush();
                   Vulkan::SubmitInfo info{};
                   info.AddWait(first.GetWorkSemaphore()->Handle(), value);
                   second.Flush(info);
               }),
               "not a proven acyclic edge");

    Vulkan::Instance instance(0, false);
    Dispatch dispatch{instance};
    constexpr u32 Slots = 300;
    HostBuffer host{instance.GetAllocator(), Slots * 4};

    {
        Vulkan::Scheduler scheduler(instance, true);
        u64 previous = 0;
        bool increasing = true;
        for (u32 i = 0; i < Slots; ++i) {
            RecordFill(scheduler, dispatch, host.buffer, i, 1, 0x1000 + i);
            const u64 value = scheduler.Flush();
            increasing = increasing && value > previous;
            previous = value;
            if (i == Slots / 2) {
                scheduler.Wait(value); // a value most likely still queued for vkQueueSubmit
                Expect("Wait on a queued value: its fill and every earlier one landed", [&] {
                    for (u32 s = 0; s <= i; ++s) {
                        if (host.words[s] != 0x1000 + s) {
                            return false;
                        }
                    }
                    return true;
                }());
            }
        }
        scheduler.Finish();
        bool all = true;
        for (u32 s = 0; s < Slots; ++s) {
            all = all && host.words[s] == 0x1000 + s;
        }
        Expect("300 consecutive small submissions: distinct fills all landed", all);
        Expect("  ... their values increase", increasing);

        // Direct recording between asynchronous submissions.
        RecordFill(scheduler, dispatch, host.buffer, 0, 1, 0xA1);
        scheduler.Flush();
        const auto* d = &dispatch.d;
        scheduler.CommandBuffer().fillBuffer(vk::Buffer{host.buffer}, 4, 4, 0xA2, *d);
        scheduler.Flush();
        RecordFill(scheduler, dispatch, host.buffer, 2, 1, 0xA3);
        scheduler.Finish();
        Expect("direct recording between asynchronous submissions",
               host.words[0] == 0xA1 && host.words[1] == 0xA2 && host.words[2] == 0xA3);
        scheduler.Close();
    }

    {
        // The packet cap: the recorder is held inside the first submission's commands.
        Vulkan::Scheduler scheduler(instance, true);
        std::atomic<bool> release{false};
        RecordHold(scheduler, &release);
        constexpr u32 Cap = Vulkan::Scheduler::MaxQueuedSubmits;
        std::atomic<u32> flushed{0};
        std::thread producer([&] {
            for (u32 i = 0; i < Cap + 1; ++i) {
                RecordFill(scheduler, dispatch, host.buffer, 10 + i, 1, 0xC0 + i);
                scheduler.Flush();
                flushed.fetch_add(1);
            }
        });
        std::this_thread::sleep_for(300ms);
        const u32 before = flushed.load();
        release.store(true);
        producer.join();
        Expect("packet cap: with the recorder held, MaxQueuedSubmits Flushes return, the next waits",
               before == Cap);
        scheduler.Finish();
        bool all = true;
        for (u32 i = 0; i < Cap + 1; ++i) {
            all = all && host.words[10 + i] == 0xC0 + i;
        }
        Expect("  ... after the release all of them landed", all);
        scheduler.Close();
    }

    {
        // A delayed producer on another scheduler.
        Vulkan::Scheduler producer(instance, true);
        Vulkan::Scheduler consumer(instance, false);
        host.words[30] = 0; // written by the first block
        std::atomic<bool> release{false};
        RecordHold(producer, &release);
        RecordFill(producer, dispatch, host.buffer, 30, 1, 0xD1);
        const u64 value = producer.Flush();
        std::atomic<bool> consumer_flushed{false};
        std::thread waiter([&] {
            RecordFill(consumer, dispatch, host.buffer, 30, 1, 0xD2);
            Vulkan::SubmitInfo info{};
            info.AddWait(producer.GetWorkSemaphore()->Handle(), value);
            consumer.Flush(info);
            consumer_flushed.store(true);
        });
        std::this_thread::sleep_for(300ms);
        const bool held = !consumer_flushed.load();
        release.store(true);
        waiter.join();
        consumer.Finish();
        producer.Finish();
        Expect("delayed producer: the waiting submission is not queued before its producer",
               held);
        Expect("  ... and its fill lands after the producer's", host.words[30] == 0xD2);
        producer.Close();
        consumer.Close();
    }

    {
        // The host-copy prefix: an earlier held copy delays its submission; a later one does
        // not.
        Vulkan::Scheduler scheduler(instance, true);
        host.words[40] = host.words[41] = 0; // written by the first block
        std::atomic<bool> release_earlier{false}, release_later{false};
        scheduler.RecordHostCopy([&] {
            BbCopy::Async([&] {
                while (!release_earlier.load()) {
                    std::this_thread::yield();
                }
            });
        });
        RecordFill(scheduler, dispatch, host.buffer, 40, 1, 0xE1);
        const u64 first = scheduler.Flush();
        scheduler.RecordHostCopy([&] {
            BbCopy::Async([&] {
                while (!release_later.load()) {
                    std::this_thread::yield();
                }
            });
        });
        RecordFill(scheduler, dispatch, host.buffer, 41, 1, 0xE2);
        scheduler.KickRecording(true);
        std::this_thread::sleep_for(300ms);
        const bool held = host.words[40] == 0 && !scheduler.IsFree(first);
        release_earlier.store(true);
        scheduler.Wait(first);
        Expect("host copies: a submission waits for its earlier held copy", held);
        Expect("  ... and is submitted once it finishes, a later copy still held",
               host.words[40] == 0xE1 && !release_later.load());
        release_later.store(true);
        scheduler.Finish();
        scheduler.Close();
    }

    {
        // The synchronous fallback with a submission still queued.
        Vulkan::Scheduler scheduler(instance, true);
        host.words[60] = 0;
        std::atomic<bool> release{false};
        RecordHold(scheduler, &release);
        RecordFill(scheduler, dispatch, host.buffer, 60, 1, 0x61);
        scheduler.Flush(); // queued behind the held recorder
        __atomic_or_fetch(&runtime_disabled_optimizations, BbToggle::ThreadedRecording,
                          __ATOMIC_RELAXED);
        std::atomic<bool> finished{false};
        std::thread finisher([&] {
            scheduler.Finish(); // synchronous now: must drain the queued packet first
            finished.store(true);
        });
        std::this_thread::sleep_for(200ms);
        release.store(true);
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!finished.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(10ms);
        }
        const bool done = finished.load();
        Expect("synchronous fallback with a queued submission: Finish completes (no deadlock)",
               done);
        if (!done) {
            std::printf("  deadlocked: stopping\n");
            std::fflush(stdout);
            std::_Exit(1);
        }
        finisher.join();
        Expect("  ... and the queued submission's fill landed", host.words[60] == 0x61);
        __atomic_and_fetch(&runtime_disabled_optimizations, ~BbToggle::ThreadedRecording,
                           __ATOMIC_RELAXED);
        scheduler.Close();
    }

    {
        // A delayed producer through WaitBeforeNextSubmit.
        Vulkan::Scheduler producer(instance, true);
        Vulkan::Scheduler consumer(instance, false);
        host.words[61] = 0;
        std::atomic<bool> release{false};
        RecordHold(producer, &release);
        RecordFill(producer, dispatch, host.buffer, 61, 1, 0x71);
        const u64 value = producer.Flush();
        std::atomic<bool> consumer_flushed{false};
        std::thread waiter([&] {
            RecordFill(consumer, dispatch, host.buffer, 61, 1, 0x72);
            consumer.WaitBeforeNextSubmit(producer.GetWorkSemaphore()->Handle(), value);
            consumer.Flush();
            consumer_flushed.store(true);
        });
        std::this_thread::sleep_for(300ms);
        const bool held = !consumer_flushed.load();
        release.store(true);
        waiter.join();
        consumer.Finish();
        producer.Finish();
        Expect("WaitBeforeNextSubmit on a delayed producer: not queued before it", held);
        Expect("  ... and its fill lands after the producer's", host.words[61] == 0x72);
        producer.Close();
        consumer.Close();
    }

    {
        // The pool cap: the GPU held on an external timeline semaphore. The cap is reachable only
        // because the instance raises MoltenVK's Metal command buffers in flight above it
        // (vk_platform.cpp); at MoltenVK's default of 64, vkQueueSubmit blocks first and the held
        // command buffer ends in a Metal GPU timeout (device lost).
        const vk::Device device = instance.GetDevice();
        const vk::SemaphoreTypeCreateInfo timeline_type{
            .semaphoreType = vk::SemaphoreType::eTimeline, .initialValue = 0};
        const auto [sem_result, gate] =
            device.createSemaphore({.pNext = &timeline_type}, nullptr, dispatch.d);
        Expect("pool cap: external timeline semaphore created", sem_result == vk::Result::eSuccess);
        Vulkan::Scheduler scheduler(instance, true);
        scheduler.WaitBeforeNextSubmit(gate, 1);
        RecordFill(scheduler, dispatch, host.buffer, 62, 1, 0x80);
        scheduler.Flush(); // the GPU waits here until the gate is signalled
        constexpr u32 Extra = Vulkan::CommandPool::MaxBuffers + 40;
        std::atomic<bool> produced{false};
        std::thread producer([&] {
            for (u32 i = 0; i < Extra; ++i) {
                RecordFill(scheduler, dispatch, host.buffer, 63, 1, 0x81 + i);
                scheduler.Flush();
            }
            produced.store(true);
        });
        // A synchronized observation that the recorder is blocked at the cap (not a sleep).
        auto deadline = std::chrono::steady_clock::now() + 20s;
        while (scheduler.CommandBufferWaitsAtCap() == 0 &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(5ms);
        }
        const bool at_cap = scheduler.CommandBufferWaitsAtCap() > 0;
        const size_t buffers = scheduler.CommandBuffersAllocated();
        Expect("pool cap: with the GPU held a commit waits at the cap", at_cap);
        Expect("  ... with exactly MaxBuffers command buffers",
               buffers == Vulkan::CommandPool::MaxBuffers);
        const vk::SemaphoreSignalInfo signal{.semaphore = gate, .value = 1};
        Expect("  ... signalled", device.signalSemaphore(signal, dispatch.d) ==
                                      vk::Result::eSuccess);
        // Bounded completion after the release (a stale GPU tick in the pool would spin here).
        deadline = std::chrono::steady_clock::now() + 20s;
        while (!produced.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(5ms);
        }
        const bool completed = produced.load();
        Expect("  ... and every submission completes within 20 s of the release", completed);
        if (!completed) {
            std::printf("  hung after the release: stopping\n");
            std::fflush(stdout);
            std::_Exit(1);
        }
        producer.join();
        scheduler.Finish();
        Expect("  ... every fill landed",
               host.words[62] == 0x80 && host.words[63] == 0x81 + Extra - 1);
        scheduler.Close();
        device.destroySemaphore(gate, nullptr, dispatch.d);
    }

    {
        // Close with queued submissions.
        Vulkan::Scheduler scheduler(instance, true);
        std::atomic<bool> release{false};
        RecordHold(scheduler, &release);
        host.words[50] = host.words[51] = host.words[52] = 0; // written by the first block
        // At most MaxQueuedSubmits fit while the recorder is held (one more Flush would wait for
        // space this thread frees only after it).
        constexpr u32 Queued = std::min<u32>(3, Vulkan::Scheduler::MaxQueuedSubmits);
        for (u32 i = 0; i < Queued; ++i) {
            RecordFill(scheduler, dispatch, host.buffer, 50 + i, 1, 0xF0 + i);
            scheduler.Flush();
        }
        std::thread releaser([&] {
            std::this_thread::sleep_for(100ms);
            release.store(true);
        });
        scheduler.Close();
        releaser.join();
        bool all_queued = true;
        for (u32 i = 0; i < Queued; ++i) {
            all_queued = all_queued && host.words[50 + i] == 0xF0 + i;
        }
        Expect("Close with queued submissions submits them all", all_queued);
        const u64 last = scheduler.CurrentTick() - 1;
        scheduler.WaitQueueSubmitted(last);
        Expect("  ... and WaitQueueSubmitted for a submitted value still succeeds after it", true);
    }

    std::printf("scheduler-async-submit-test: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
