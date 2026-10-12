// bbport (macOS): when a flip is published to the guest (flip status and count, flip event, the
// previous buffer's release). The default is at GPU retirement of the frame copy, the
// presenter's last read of the guest framebuffer (docs/macos/gpu-idle.md). Publishing when the
// flip is enqueued (the old behaviour) remains for diagnosis: BB_FLIP_AT_GPU_RETIRE=0 for the
// whole run, or toggle bit 60 (FlipAtEnqueue) live.
#pragma once
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace BbFlip {

enum class RunMode {
    Default,   ///< at GPU retirement; toggle bit 60 switches to at enqueue live
    AtRetire,  ///< BB_FLIP_AT_GPU_RETIRE=1: for the whole run, whatever bit 60 says
    AtEnqueue, ///< BB_FLIP_AT_GPU_RETIRE=0: for the whole run, whatever bit 60 says
};

/// BB_FLIP_AT_GPU_RETIRE: unset or empty, "1" or "0". Anything else stops.
inline RunMode ParseRunMode(const char* env) {
    if (!env || !*env) {
        return RunMode::Default;
    }
    if (!std::strcmp(env, "1")) {
        return RunMode::AtRetire;
    }
    if (!std::strcmp(env, "0")) {
        return RunMode::AtEnqueue;
    }
    std::fprintf(stderr, "STOP: BB_FLIP_AT_GPU_RETIRE=%s (0, 1 or unset expected)\n", env);
    std::abort();
}

/// Whether a flip submitted now is published at GPU retirement.
inline bool AtRetire(RunMode run, bool at_enqueue_bit) {
    switch (run) {
    case RunMode::AtRetire:
        return true;
    case RunMode::AtEnqueue:
        return false;
    case RunMode::Default:
        break;
    }
    return !at_enqueue_bit;
}

} // namespace BbFlip
