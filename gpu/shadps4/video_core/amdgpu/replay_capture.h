// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <chrono>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include "common/types.h"

namespace AmdGpu {
class CmdSnapshot;
}
namespace Vulkan {
class Rasterizer;
}

/// Capture for offline replay (docs/macos/capture-replay.md), Apple-only diagnostic, off unless
/// BB_REPLAY_CAPTURE is set. v1 feasibility gate, inventory stage:
///   BB_REPLAY_CAPTURE=inventory:<frames>   arm at the <frames>th retired frame end (1..100000)
///   BB_REPLAY_HOLD_MS=<ms>                 hold bound, 1..60000 (default 3000)
///   BB_REPLAY_TRIGGER=<file>               count <frames> from the poll that finds and removes
///                                          this file (every 30 retired frame ends, so up to 30
///                                          after it appears), not from launch
/// A malformed value disables the capture loudly. A retired frame end is a sceGnmSubmitDone
/// after which GpuIdle was raised with nothing queued; a frame end that found submissions already
/// queued is not counted, so the held frame can be later than <frames> + 1.
/// At the arming frame end the GPU command thread stops parsing submissions (blocking host
/// commands still run) until the guest ends the next frame (sceGnmSubmitDone), so that frame is
/// fully submitted while nothing of it has executed: the cut. The guest's own frame throttle
/// parks it. A CPU (non-EOP) flip ends the hold at once and rejects the cut: it would present a
/// framebuffer whose draws are held. The inventory (G1 cut reachability, G2 sizes and
/// unsupported inputs) is printed, nothing is written, and the held frame is released. The best
/// verdict is "no inventoried blocker found; capture NOT yet qualified": the inventory cannot
/// prove the rest of the protocol (guest CPU/HLE writers, untracked memory, image writeback).
namespace ReplayCapture {

/// Whether a capture is configured and not finished (cheap; false when unset).
bool Armed();

/// GPU command thread, after GpuIdle was raised for a frame end (sceGnmSubmitDone) with all
/// work retired. True if the hold starts now; the caller then parses no submission until
/// HoldEnded() (host commands still run).
bool OnFrameRetired(Vulkan::Rasterizer* rasterizer);

/// The hold bound left, while holding; never negative (zero once the bound has passed, which the
/// caller tests for).
std::chrono::steady_clock::duration HoldRemaining();

/// What the GPU command thread held, read when the hold ends. No queue task is resumed during
/// the hold, so the queued task counts are the work submitted since the cut, as observed when
/// each queue was sampled; `asc_seen` (AscSubmitted() read before sampling) lets HoldEnded count
/// asynchronous-compute submissions that arrive after sampling (up to its report checkpoint;
/// later ones are reported when the window closes).
struct Held {
    bool frame_ended;         ///< the guest ended the held frame (sceGnmSubmitDone)
    const char* ended_early;  ///< why the hold ended before that, if not the bound (else nullptr)
    std::span<const std::shared_ptr<AmdGpu::CmdSnapshot>> gfx; ///< unparsed gfx submits, in order
    u32 gfx_tasks;            ///< tasks queued on the graphics queue
    u32 asc_tasks;            ///< tasks queued on asynchronous-compute queues at sampling
    u64 asc_seen;             ///< AscSubmitted() before the queues were sampled
    u32 host_commands;        ///< host commands run during the hold
};

/// GPU command thread, when the hold ends. Prints the inventory and releases the hold.
void HoldEnded(Vulkan::Rasterizer* rasterizer, const Held& held);

/// Any thread, after an asynchronous-compute submission was queued: counts it while a capture
/// is in progress (monotonic, so no submission falls between two windows).
void NoteAscSubmit();

/// The asynchronous-compute submissions counted so far.
u64 AscSubmitted();

/// GPU command thread, at the GpuIdle after the released frame: closes the capture window.
void OnReleasedFrameRetired();

namespace Detail {

/// Decimal digits only (no sign, space or suffix), at most `max`; nullopt otherwise.
std::optional<u64> ParseBounded(std::string_view text, u64 max);

/// A raw scan of copied PM4 regions: potential packets, not executed ones (untaken COND_EXEC and
/// predicated ranges are counted too). A region whose scan stops early leaves the census
/// incomplete.
struct Pm4Census {
    std::array<u64, 256> opcodes{};
    u64 packets = 0, type0 = 0, type2 = 0;
    u64 regions = 0, constant_regions = 0, words = 0;
    u64 bad_header_regions = 0; ///< scan stopped at a type-1 header
    u64 truncated_regions = 0;  ///< scan stopped at a packet extending past the region
    u64 uncopied_regions = 0;   ///< not copied at submit, so not scanned
    bool Complete() const {
        return !bad_header_regions && !truncated_regions && !uncopied_regions;
    }
};

/// Scans one copied region into `census`.
void CountRegion(Pm4Census& census, std::span<const u32> words);

} // namespace Detail
} // namespace ReplayCapture
