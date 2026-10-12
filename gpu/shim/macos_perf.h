// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: BB_PERF=1 — coarse macOS performance windows (docs/macos/perf.md). GPU time is taken
// only from the first and last timestamp of each submission: MoltenVK timestamps between
// commands of one command buffer do not attribute work correctly (tests/macos_perf).
#pragma once
#ifdef __APPLE__
#include <cstdint>
#include <memory>
// The GPU library's Vulkan configuration (dynamic dispatcher, no prototypes, beta extensions).
#include "video_core/renderer_vulkan/vk_common.h"

namespace BbPerf {
/// BB_PERF=1 (requires BB_PERF_OUT). Anything else but unset/0 is an error.
bool Enabled();
/// BB_PERF_PHASES=1: submit at each phase boundary, so every phase is its own measured
/// submission. Perturbs the run; its windows are marked "perturbed".
bool Phases();
enum class Phase : unsigned { Scene, Post, Upscaler, Present };
inline constexpr unsigned NumPhases = 4;

/// Timestamps around every submission of one scheduler.
class GpuStream {
    struct Impl;
    std::unique_ptr<Impl> impl;
public:
    GpuStream(VkPhysicalDevice physical, VkDevice device, uint32_t family,
              VkSemaphore completion, bool present);
    ~GpuStream();
    /// First command of a new command buffer: the submission's work is labelled phase/frame.
    void Begin(VkCommandBuffer command, Phase phase, uint64_t frame);
    /// Last command before vkEndCommandBuffer.
    void End(VkCommandBuffer command);
    /// After vkQueueSubmit: the submission completes when the timeline reaches `tick`.
    void Submitted(uint64_t tick);
};
/// GPU execution of the main stream's completed submissions since the start (sum of each
/// submission's first-to-last timestamp span, ms) and their count; {0, 0} unless Enabled().
/// Submissions are collected at later submissions, so a total lags the newest work (the perf
/// replay waits for each iteration's GPU work and reads the delta at the next one).
struct GpuTotals {
    double main_ms;
    uint64_t main_submissions;
};
GpuTotals Totals();
/// Called once per guest flip, not per draw. Its interval includes blocking and pacing.
void Flip();
void ImportWait(uint64_t nanoseconds);
/// CPU time of the recorder thread while it executes one chunk.
class RecorderTimer {
    uint64_t start = 0;
public:
    RecorderTimer();
    ~RecorderTimer();
};
} // namespace BbPerf
#endif
