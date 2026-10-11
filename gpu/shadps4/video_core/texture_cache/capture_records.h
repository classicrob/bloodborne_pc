// SPDX-License-Identifier: GPL-2.0-or-later
// bbport (Apple): what the frame capture (docs/macos/capture-replay.md) learns about images whose
// newest content exists only in the texture cache (GPU-modified), at the capture point.
#pragma once

#include "common/types.h"

namespace VideoCore {

struct CaptureImageRecord {
    VAddr addr;
    u64 size;
    u32 pixel_format; ///< vk::Format of the cached image (the cache keeps no guest dfmt/nfmt)
    u32 tile_mode;    ///< AmdGpu::TileMode
    u32 aspects;      ///< vk::ImageAspectFlags
    u32 samples;
    u32 mips;
    u32 layers;
    /// nullptr: can be written back exactly (TileImage); else why not: "msaa", "depth+stencil",
    /// "metadata", "bc gpu-written", "scene proxy".
    const char* reject;
};

} // namespace VideoCore
