// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <memory>
#include <span>
#include "common/types.h"

namespace AmdGpu {
class CmdSnapshot;
}
namespace Vulkan {
class Rasterizer;
}

/// Capture for offline replay (docs/macos/capture-replay.md), Apple-only diagnostic, off unless
/// BB_REPLAY_CAPTURE is set. v1 feasibility gate, inventory stage:
///   BB_REPLAY_CAPTURE=inventory:<frames>   arm after <frames> sceGnmSubmitDone frame ends
///   BB_REPLAY_HOLD_MS=<ms>                 hold bound (default 3000)
/// At the arming frame end, once all its work has retired (GpuIdle), the GPU command thread stops
/// parsing submissions (host commands still run) until the guest ends the next frame
/// (sceGnmSubmitDone), so that frame is fully submitted while nothing of it has executed: the cut.
/// The guest's own frame throttle parks it. The inventory (G1 cut reachability, G2 sizes and
/// unsupported inputs) is printed, nothing is written, and the held frame is released. A cut that
/// cannot be reached within the bound, or that is not quiescent, is reported as rejected.
namespace ReplayCapture {

/// Whether a capture is configured and not finished (cheap; false when unset).
bool Armed();

/// GPU command thread, after GpuIdle was raised for a frame end (sceGnmSubmitDone) with all
/// work retired. True if the hold starts now; the caller then parses no submission until
/// HoldEnded() (host commands still run).
bool OnFrameRetired(Vulkan::Rasterizer* rasterizer);

/// The hold bound left, while holding.
std::chrono::steady_clock::duration HoldRemaining();

/// GPU command thread, when the hold ends: the guest ended the held frame (`frame_ended`) or the
/// bound passed. `held` are the submissions received during the hold, in order. Prints the
/// inventory and releases the hold.
void HoldEnded(Vulkan::Rasterizer* rasterizer, bool frame_ended,
               std::span<const std::shared_ptr<AmdGpu::CmdSnapshot>> held, u32 host_commands);

/// Any thread: an asynchronous-compute submission arrived (v1 rejects one inside the window).
void NoteAscSubmit();

/// GPU command thread, at the GpuIdle after the released frame: closes the capture window.
void OnReleasedFrameRetired();

} // namespace ReplayCapture
