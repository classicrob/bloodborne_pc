// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>
#include "video_core/amdgpu/liverpool.h"

namespace AmdGpu {

/// Liverpool's parse state for the perf replay (a friend of Liverpool): read at the cut by the
/// capture (replay_perf_capture.cpp) and written back before every replayed iteration
/// (gpu/shim/bbport_replay.cpp), while the GPU command thread waits for a submission.
class ReplayStateAccess {
public:
    static const Liverpool::ConstantEngine& Ce(const Liverpool& lp) {
        return lp.cblock;
    }
    static const ComputeProgram& CsState(const Liverpool& lp, u32 queue) {
        return lp.mapped_queues[queue].cs_state;
    }
    static VAddr IndirectArgs(const Liverpool& lp) {
        return lp.indirect_args_addr;
    }
    static u32 CounterPairs(const Liverpool& lp) {
        return lp.num_counter_pairs;
    }
    static Vulkan::Rasterizer* Rasterizer(const Liverpool& lp) {
        return lp.rasterizer;
    }
    /// The constant engine's RAM (one per process).
    static std::span<u8> CeHeap() {
        return Liverpool::ConstantEngine::constants_heap;
    }

    static void SetCeCounters(Liverpool& lp, u32 compare, u32 ce, u32 de) {
        lp.cblock.ce_compare_count = compare;
        lp.cblock.ce_count = ce;
        lp.cblock.de_count = de;
    }
    static void SetCsState(Liverpool& lp, u32 queue, const ComputeProgram& cs) {
        lp.mapped_queues[queue].cs_state = cs;
    }
    static void SetIndirectArgs(Liverpool& lp, VAddr address) {
        lp.indirect_args_addr = address;
    }
};

} // namespace AmdGpu
