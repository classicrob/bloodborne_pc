// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <span>
#include <string>
#include "common/types.h"

namespace AmdGpu {
class CmdSnapshot;
struct Liverpool;
} // namespace AmdGpu

/// Perf replay v0 capture writer (docs/macos/capture-replay.md "Perf replay v0"), Apple-only.
namespace ReplayCapture::Perf {

/// GPU command thread, at the end of a hold whose cut has no blocker other than image
/// authority: writes the capture of the held frame (`held`, in submission order) under
/// BB_REPLAY_DIR (default out/replay). Returns its directory, or "" with `why` (loud, nothing
/// partial is left as a capture: a failed directory is renamed *.incomplete).
std::string Write(const AmdGpu::Liverpool& liverpool,
                  std::span<const std::shared_ptr<AmdGpu::CmdSnapshot>> held, std::string& why);

} // namespace ReplayCapture::Perf
