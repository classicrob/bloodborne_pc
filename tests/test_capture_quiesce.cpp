// SPDX-License-Identifier: GPL-2.0-or-later
// The frame capture's completion check (Scheduler::WaitQuiescentForCapture, what
// Rasterizer::QuiesceForCapture does after Finish) on a real device: work that runs after GPU
// completion on other threads keeps the cut open until it has run to the end.
// 1. A priority operation the runner has dequeued and is running (held after GPU completion).
// 2. A label its writer has taken and not yet written (held before its write), with
//    OrderedGuestWrites off, where WaitDeferredSignals returns at once.
// Each must make the check fail while held and pass once released.
// Also, on a scheduler recording on its own thread (where labels batch): whether the unsubmitted
// label batch holds one of the first N labels, which the parse split's ordered-WRITE_DATA census
// reports (bbport_parse_split.h).
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include "bbport_toggles.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace {
unsigned failures = 0;
void Expect(const char* what, bool ok, const std::string& detail = {}) {
    std::fprintf(stderr, "%-70s %s%s%s\n", what, ok ? "ok" : "FAIL", detail.empty() ? "" : ": ",
                 detail.c_str());
    failures += ok ? 0 : 1;
}

/// A callback that reports it started and then waits until released.
struct Hold {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false, released = false;
    void Block() {
        std::unique_lock lock{mutex};
        entered = true;
        cv.notify_all();
        cv.wait(lock, [this] { return released; });
    }
    bool WaitEntered() {
        std::unique_lock lock{mutex};
        return cv.wait_for(lock, std::chrono::seconds(10), [this] { return entered; });
    }
    void Release() {
        {
            std::scoped_lock lock{mutex};
            released = true;
        }
        cv.notify_all();
    }
};
} // namespace

int main() {
    // Labels at GPU completion for the whole run (read when the first label is issued).
    setenv("BB_LABELS_AT_GPU_COMPLETE", "1", 1);
    Vulkan::Instance instance(0, false);
    Vulkan::Scheduler scheduler(instance);
    std::string why;

    Expect("nothing issued: quiescent", scheduler.WaitQuiescentForCapture(
                                            std::chrono::milliseconds(100), why), why);

    {
        Hold hold;
        scheduler.DeferPriorityOperation([&] { hold.Block(); });
        scheduler.Finish(); // its tick completes: the runner dequeues and runs it
        Expect("priority operation started after GPU completion", hold.WaitEntered());
        why.clear();
        const bool quiet = scheduler.WaitQuiescentForCapture(std::chrono::milliseconds(50), why);
        Expect("held priority operation keeps the cut open", !quiet &&
                   why.find("priority operations not finished") != std::string::npos, why);
        hold.Release();
        why.clear();
        Expect("released priority operation: quiescent",
               scheduler.WaitQuiescentForCapture(std::chrono::seconds(2), why), why);
    }

    {
        __atomic_store_n(&runtime_disabled_optimizations, BbToggle::OrderedGuestWrites,
                         __ATOMIC_RELEASE);
        Hold hold;
        scheduler.SignalAtGpuCompletion([&] { hold.Block(); });
        scheduler.Finish();
        Expect("label taken by its writer after GPU completion", hold.WaitEntered());
        const auto started = std::chrono::steady_clock::now();
        scheduler.WaitDeferredSignals(); // OrderedGuestWrites off: returns at once
        Expect("WaitDeferredSignals does not wait (OrderedGuestWrites off)",
               std::chrono::steady_clock::now() - started < std::chrono::milliseconds(500));
        why.clear();
        const bool quiet = scheduler.WaitQuiescentForCapture(std::chrono::milliseconds(50), why);
        Expect("taken, unwritten label keeps the cut open", !quiet &&
                   why.find("deferred signals (labels) not written") != std::string::npos, why);
        hold.Release();
        why.clear();
        Expect("written label: quiescent",
               scheduler.WaitQuiescentForCapture(std::chrono::seconds(2), why), why);
        __atomic_store_n(&runtime_disabled_optimizations, 0, __ATOMIC_RELEASE);
    }

    {
        Vulkan::Scheduler threaded(instance, true);
        std::atomic<int> written{0};
        threaded.SignalAtGpuCompletion([&] { written.fetch_add(1); }, true); // label 1
        const bool first = threaded.LabelsBatched() && threaded.LabelsBatchedThrough(1) &&
                           !threaded.LabelsBatchedThrough(0);
        threaded.FlushLabels(true);
        const bool submitted = !threaded.LabelsBatched() && !threaded.LabelsBatchedThrough(1);
        threaded.SignalAtGpuCompletion([&] { written.fetch_add(1); }, true); // label 2
        const bool later_only = threaded.LabelsBatched() && !threaded.LabelsBatchedThrough(1) &&
                                threaded.LabelsBatchedThrough(2);
        threaded.FlushLabels(true);
        threaded.Finish();
        for (int i = 0; i < 5000 && written.load() < 2; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        Expect("label batch: holds the first label once batched", first);
        Expect("label batch: none after FlushLabels", submitted);
        Expect("label batch: a new batch holds only the later label", later_only);
        Expect("label batch: both labels written", written.load() == 2);
    }

    std::fprintf(stderr, "capture-quiesce-test: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
