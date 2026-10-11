// bbport: GPU-side assertion failures stop the port with exit code 23.
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <atomic>
#include "common/assert.h"
#include "common/assert_hook.h"
#include "common/logging/log.h"

namespace {
std::atomic<void (*)()> stop_hook{nullptr};
std::atomic_flag stop_hook_ran = ATOMIC_FLAG_INIT;
} // namespace

void SetAssertStopHook(void (*hook)()) {
    stop_hook.store(hook);
}

void assert_fail_impl() {
    if (const auto hook = stop_hook.load(); hook && !stop_hook_ran.test_and_set()) {
        hook();
    }
    std::fflush(stdout);
    std::fputs("STOP: GPU library assertion failed (see GPU log above)\n", stderr);
    std::_Exit(23);
}

[[noreturn]] void unreachable_impl() {
    assert_fail_impl();
    throw std::runtime_error("Unreachable code");
}

void assert_fail_debug_msg(const char* msg) {
    LOG_CRITICAL(Debug, "Assertion Failed!\n{}", msg);
    assert_fail_impl();
}
