// SPDX-License-Identifier: GPL-2.0-or-later
// BbFlip::Publisher (gpu/shim/bbport_flip_publish.h): flips published in flip order, each once the
// GPU retired its frame copy when the mode is on, at once when it is off; across a live switch no
// flip overtakes an earlier one; a blank flip (its own copy submission) waits for its tick too;
// stopping reports the flips left. The GPU timeline is simulated here.
#include <atomic>
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>
#include "bbport_flip_mode.h"
#include "bbport_flip_publish.h"

namespace {
unsigned failures = 0;
void Expect(const char* what, bool ok) {
    std::fprintf(stderr, "%-72s %s\n", what, ok ? "ok" : "FAIL");
    failures += ok ? 0 : 1;
}

struct Gpu {
    std::mutex mutex;
    std::condition_variable_any cv;
    std::uint64_t retired = 0;
    bool Wait(std::uint64_t tick, const std::stop_token& stop) {
        std::unique_lock lock{mutex};
        return cv.wait(lock, stop, [&] { return retired >= tick; });
    }
    void Retire(std::uint64_t tick) {
        {
            std::scoped_lock lock{mutex};
            retired = tick;
        }
        cv.notify_all();
    }
};

struct Log {
    std::mutex mutex;
    std::vector<char> published;
    std::vector<char> Get() {
        std::scoped_lock lock{mutex};
        return published;
    }
    void Add(char c) {
        std::scoped_lock lock{mutex};
        published.push_back(c);
    }
};

bool Eventually(const auto& condition) {
    for (int i = 0; i < 400; ++i) {
        if (condition()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return condition();
}
void Settle() {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}
} // namespace

int main() {
    Gpu gpu;
    Log log;
    {
        BbFlip::Publisher<char> publisher{
            [&](std::uint64_t tick, const std::stop_token& stop) { return gpu.Wait(tick, stop); },
            [&](const char& flip) { log.Add(flip); }};

        publisher.Submit('a', 10, false);
        Expect("mode off, nothing queued: published at once", log.Get() == std::vector{'a'});

        publisher.Submit('b', 2, true);
        Settle();
        Expect("mode on: not published before its frame copy retired",
               log.Get() == std::vector{'a'});
        gpu.Retire(2);
        Expect("  ... published once it retired",
               Eventually([&] { return log.Get() == std::vector{'a', 'b'}; }));

        // Live switch on -> off: C waits for tick 4; D, submitted with the mode off, must not
        // overtake it.
        publisher.Submit('c', 4, true);
        publisher.Submit('d', 0, false);
        Settle();
        Expect("switch on -> off: the later flip waits behind the earlier one",
               log.Get() == std::vector{'a', 'b'});
        gpu.Retire(4);
        Expect("  ... then both, in flip order",
               Eventually([&] { return log.Get() == std::vector{'a', 'b', 'c', 'd'}; }));

        // A blank flip has its own copy submission: with the mode on it waits for that too.
        publisher.Submit('e', 6, true);
        Settle();
        Expect("blank flip with the mode on waits for its own submission",
               log.Get().size() == 4);
        gpu.Retire(6);
        Expect("  ... and is published after it retired",
               Eventually([&] { return log.Get().size() == 5; }));

        // Stopping with flips left: none is published after the stop, and they are reported.
        publisher.Submit('f', 8, true);
        publisher.Submit('g', 9, true);
        Settle();
        const std::size_t left = publisher.Stop();
        gpu.Retire(9);
        Settle();
        Expect("stop with flips pending: both reported, neither published",
               left == 2 && log.Get().size() == 5);
    }
    // When flips are published (bbport_flip_mode.h): at GPU retirement by default, toggle bit 60
    // switches to at enqueue live, BB_FLIP_AT_GPU_RETIRE=0/1 fixes the mode for the run.
    {
        using BbFlip::RunMode;
        const RunMode unset = BbFlip::ParseRunMode(nullptr);
        const RunMode empty = BbFlip::ParseRunMode("");
        const RunMode on = BbFlip::ParseRunMode("1");
        const RunMode off = BbFlip::ParseRunMode("0");
        Expect("flip mode: unset or empty is the default, 1 and 0 fix the run",
               unset == RunMode::Default && empty == RunMode::Default &&
                   on == RunMode::AtRetire && off == RunMode::AtEnqueue);
        Expect("  ... default: at retirement, bit 60 set: at enqueue",
               BbFlip::AtRetire(unset, false) && !BbFlip::AtRetire(unset, true));
        Expect("  ... =1 and =0 ignore bit 60",
               BbFlip::AtRetire(on, true) && BbFlip::AtRetire(on, false) &&
                   !BbFlip::AtRetire(off, false) && !BbFlip::AtRetire(off, true));
        std::fflush(stderr);
        const pid_t child = fork();
        if (child == 0) {
            std::freopen("/dev/null", "w", stderr);
            BbFlip::ParseRunMode("yes");
            std::_Exit(0);
        }
        int status = 0;
        waitpid(child, &status, 0);
        Expect("  ... any other value stops", WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
    }
    std::fprintf(stderr, "%s: %u failures\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
