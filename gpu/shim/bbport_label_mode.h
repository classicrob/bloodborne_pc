// bbport (macOS): when GPU labels (EOP, EOS, compute RELEASE_MEM) and their interrupts are written.
// The default is at GPU completion: once the GPU has completed the work before them (labels at
// completion removed the corrupted frames on PREVIEW-17). Writing them as soon as the work is
// recorded ("at submit", the old behaviour) remains for diagnosis: BB_LABELS_AT_GPU_COMPLETE=0
// for the whole run, or toggle bit 59 (LabelsAtSubmit) live (docs/macos/gpu-idle.md).
#pragma once
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace BbLabels {

enum class RunMode {
    Default,       ///< at GPU completion; toggle bit 59 switches to at submit live
    AtGpuComplete, ///< BB_LABELS_AT_GPU_COMPLETE=1: for the whole run, whatever bit 59 says
    AtSubmit,      ///< BB_LABELS_AT_GPU_COMPLETE=0: for the whole run, whatever bit 59 says
};

/// BB_LABELS_AT_GPU_COMPLETE: unset or empty, "1" or "0". Anything else stops.
inline RunMode ParseRunMode(const char* env) {
    if (!env || !*env) {
        return RunMode::Default;
    }
    if (!std::strcmp(env, "1")) {
        return RunMode::AtGpuComplete;
    }
    if (!std::strcmp(env, "0")) {
        return RunMode::AtSubmit;
    }
    std::fprintf(stderr, "STOP: BB_LABELS_AT_GPU_COMPLETE=%s (0, 1 or unset expected)\n", env);
    std::abort();
}

/// Whether a label issued now is written at GPU completion.
inline bool AtGpuComplete(RunMode run, bool at_submit_bit) {
    switch (run) {
    case RunMode::AtGpuComplete:
        return true;
    case RunMode::AtSubmit:
        return false;
    case RunMode::Default:
        break;
    }
    return !at_submit_bit;
}

} // namespace BbLabels
