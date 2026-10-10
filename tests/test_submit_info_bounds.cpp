// SPDX-License-Identifier: GPL-2.0-or-later
// Scheduler::SubmitInfo holds four waits and four signals. Filling them is legitimate; a fifth
// used to write past the arrays silently and must now abort.
#include <cstdio>
#include <sys/wait.h>
#include <unistd.h>
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace {
bool Aborts(void (*body)()) {
    const pid_t child = fork();
    if (child == 0) {
        freopen("/dev/null", "w", stderr);
        body();
        _exit(0);
    }
    int status = 0;
    waitpid(child, &status, 0);
    return WIFSIGNALED(status) || (WIFEXITED(status) && WEXITSTATUS(status) != 0);
}
} // namespace

int main() {
    using Vulkan::SubmitInfo;
    u32 failures = 0;
    const auto expect = [&](const char* what, bool ok) {
        std::fprintf(stderr, "%-44s %s\n", what, ok ? "ok" : "FAIL");
        failures += ok ? 0 : 1;
    };
    SubmitInfo full{};
    for (u64 i = 0; i < 4; ++i) {
        full.AddWait(vk::Semaphore{}, i + 1);
        full.AddSignal(vk::Semaphore{}, i + 1);
    }
    expect("four waits and four signals fit",
           full.num_wait_semas == 4 && full.num_signal_semas == 4 && full.wait_ticks[3] == 4);
    expect("a fifth wait aborts", Aborts([] {
               SubmitInfo info{};
               for (u64 i = 0; i < 5; ++i) info.AddWait(vk::Semaphore{}, i);
           }));
    expect("a fifth signal aborts", Aborts([] {
               SubmitInfo info{};
               for (u64 i = 0; i < 5; ++i) info.AddSignal(vk::Semaphore{}, i);
           }));
    std::fprintf(stderr, "%s: %u failures\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
