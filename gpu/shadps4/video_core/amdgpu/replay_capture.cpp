// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/amdgpu/replay_capture.h"

#ifdef __APPLE__

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <fmt/format.h>
#include "video_core/amdgpu/cmd_snapshot.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/texture_cache/capture_records.h"
#include "video_core/texture_cache/texture_cache.h"
#include "../../../../src/runtime_memory.h"

namespace ReplayCapture {
namespace {

enum class State { Off, Waiting, Holding, Released, Done };

struct Config {
    u64 arm_frames = 0;
    std::chrono::milliseconds hold_bound{3000};
    bool valid = false;
};

const Config& Settings() {
    static const Config config = [] {
        Config c;
        const char* value = std::getenv("BB_REPLAY_CAPTURE");
        if (!value || !*value) {
            return c;
        }
        const std::string text{value};
        constexpr std::string_view prefix = "inventory:";
        if (text.rfind(prefix, 0) != 0) {
            std::fprintf(stderr, "*** BB_REPLAY_CAPTURE=%s: only inventory:<frames> is implemented; "
                                 "capture disabled ***\n", value);
            return c;
        }
        char* end = nullptr;
        c.arm_frames = std::strtoull(text.c_str() + prefix.size(), &end, 10);
        if (!end || *end != '\0' || c.arm_frames == 0) {
            std::fprintf(stderr, "*** BB_REPLAY_CAPTURE=%s: <frames> must be a positive integer; "
                                 "capture disabled ***\n", value);
            return c;
        }
        if (const char* ms = std::getenv("BB_REPLAY_HOLD_MS")) {
            c.hold_bound = std::chrono::milliseconds(std::strtoull(ms, nullptr, 10));
        }
        c.valid = true;
        std::fprintf(stderr, "\n*** BB_REPLAY_CAPTURE: inventory at frame end %llu, hold bound %lld ms "
                             "(diagnostic: holds the GPU command thread for one frame) ***\n\n",
                     static_cast<unsigned long long>(c.arm_frames),
                     static_cast<long long>(c.hold_bound.count()));
        return c;
    }();
    return config;
}

std::atomic<State> state{State::Off};
std::atomic<u32> asc_submits{0};
u64 frames_seen = 0;
std::chrono::steady_clock::time_point hold_start, release_time;

/// PM4 type-3 opcodes that matter to a replay cut (capture-replay.md "Dependencies without a
/// guest CPU"), and the rest by count.
struct Pm4Census {
    std::array<u64, 256> opcodes{};
    u64 packets = 0, type0 = 0, type2 = 0, bad_regions = 0, uncopied_regions = 0;
    u64 regions = 0, constant_regions = 0, words = 0;
};

void CountRegion(Pm4Census& census, std::span<const u32> words) {
    for (size_t i = 0; i < words.size();) {
        const u32 header = words[i];
        const u32 type = header >> 30;
        if (type == 2) {
            ++census.type2;
            ++i;
            continue;
        }
        if (type == 1) { // not a valid header: the rest of this region is not a packet stream
            ++census.bad_regions;
            return;
        }
        const u32 count = (header >> 16) & 0x3FFF;
        if (type == 3) {
            ++census.opcodes[(header >> 8) & 0xFF];
            ++census.packets;
        } else {
            ++census.type0;
        }
        i += count + 2;
    }
}

const char* OpcodeName(u32 op) {
    switch (op) {
    case 0x15: return "DISPATCH_DIRECT";
    case 0x16: return "DISPATCH_INDIRECT";
    case 0x22: return "COND_EXEC";
    case 0x24: return "DRAW_INDIRECT";
    case 0x25: return "DRAW_INDEX_INDIRECT";
    case 0x27: return "DRAW_INDEX_2";
    case 0x2D: return "DRAW_INDEX_AUTO";
    case 0x33: return "INDIRECT_BUFFER_CONST";
    case 0x35: return "DRAW_INDEX_OFFSET_2";
    case 0x37: return "WRITE_DATA";
    case 0x39: return "MEM_SEMAPHORE";
    case 0x3C: return "WAIT_REG_MEM";
    case 0x3F: return "INDIRECT_BUFFER";
    case 0x40: return "COPY_DATA";
    case 0x45: return "COND_WRITE";
    case 0x46: return "EVENT_WRITE";
    case 0x47: return "EVENT_WRITE_EOP";
    case 0x48: return "EVENT_WRITE_EOS";
    case 0x50: return "DMA_DATA";
    case 0x59: return "REWIND";
    case 0x80: return "LOAD_CONST_RAM";
    case 0x81: return "WRITE_CONST_RAM";
    case 0x83: return "DUMP_CONST_RAM";
    case 0x84: return "INCREMENT_CE_COUNTER";
    case 0x85: return "INCREMENT_DE_COUNTER";
    case 0x86: return "WAIT_ON_CE_COUNTER";
    default: return nullptr;
    }
}

} // namespace

bool Armed() {
    const State s = state.load(std::memory_order_relaxed);
    if (s == State::Off) {
        if (!Settings().valid) {
            return false;
        }
        State expected = State::Off;
        state.compare_exchange_strong(expected, State::Waiting);
        return true;
    }
    return s == State::Waiting || s == State::Holding || s == State::Released;
}

bool OnFrameRetired(Vulkan::Rasterizer* rasterizer) {
    if (!Armed() || state.load() != State::Waiting || ++frames_seen < Settings().arm_frames) {
        return false;
    }
    if (!rasterizer) {
        fmt::print("Replay capture REFUSED: no rasterizer\n");
        state = State::Done;
        return false;
    }
    const std::string refusal = rasterizer->CaptureSettingsRefusal();
    if (!refusal.empty()) {
        fmt::print("Replay capture REFUSED at frame end {}: host features that make the frame depend "
                   "on host-side history are on: {}. Turn them off (docs/macos/capture-replay.md).\n",
                   frames_seen, refusal);
        std::fflush(stdout);
        state = State::Done;
        return false;
    }
    asc_submits = 0;
    hold_start = std::chrono::steady_clock::now();
    state = State::Holding;
    fmt::print("Replay capture: frame end {} retired; holding the next frame's submissions "
               "(bound {} ms)\n", frames_seen, Settings().hold_bound.count());
    std::fflush(stdout);
    return true;
}

std::chrono::steady_clock::duration HoldRemaining() {
    const auto left = hold_start + Settings().hold_bound - std::chrono::steady_clock::now();
    return std::max(left, std::chrono::steady_clock::duration::zero());
}

void NoteAscSubmit() {
    const State s = state.load(std::memory_order_relaxed);
    if (s == State::Holding || s == State::Released) {
        asc_submits.fetch_add(1, std::memory_order_relaxed);
    }
}

void HoldEnded(Vulkan::Rasterizer* rasterizer, bool frame_ended,
               std::span<const std::shared_ptr<AmdGpu::CmdSnapshot>> held, u32 host_commands) {
    const double hold_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - hold_start).count();
    std::vector<std::string> rejected;

    // G1: the cut.
    if (!frame_ended) {
        rejected.push_back(fmt::format(
            "the guest did not end the held frame within {} ms ({} submissions held): it waits on "
            "GPU work it submitted before ending the frame, so a whole-frame hold cannot be a cut",
            Settings().hold_bound.count(), held.size()));
    }
    std::string quiesce_why;
    const bool quiescent = rasterizer->QuiesceForCapture(quiesce_why);
    if (!quiescent) {
        rejected.push_back("renderer not quiescent at the cut: " + quiesce_why);
    }
    if (host_commands) {
        rejected.push_back(fmt::format("{} host commands ran during the hold (memory mapping or "
                                       "cache operations inside the cut window)", host_commands));
    }

    // G2: the held frame's command stream.
    Pm4Census census;
    u64 snapshot_bytes = 0;
    for (const auto& snapshot : held) {
        snapshot_bytes += snapshot->copied_bytes;
        for (const auto& region : snapshot->Regions()) {
            ++census.regions;
            census.constant_regions += region.constant;
            if (!region.copied) {
                ++census.uncopied_regions;
                continue;
            }
            census.words += region.size_dw;
            CountRegion(census, snapshot->Words(region));
        }
    }
    const u32 asc = asc_submits.load();
    if (asc) {
        rejected.push_back(fmt::format("{} asynchronous-compute submissions in the window (v1 has "
                                       "no ASC capture)", asc));
    }
    if (census.opcodes[0x59]) {
        rejected.push_back(fmt::format("{} REWIND packets (v1 rejects live command refetch)",
                                       census.opcodes[0x59]));
    }

    // G2: memory and image authority.
    RuntimeMemoryInventory memory{};
    const bool memory_ok = runtime_memory_inventory(&memory) == 0;
    std::vector<VideoCore::CaptureImageRecord> images;
    rasterizer->GetTextureCache().ReportGpuModifiedForCapture(images);
    u64 image_bytes = 0, flushable_bytes = 0;
    std::vector<std::pair<std::string, u32>> reasons;
    for (const auto& image : images) {
        image_bytes += image.size;
        if (!image.reject) {
            flushable_bytes += image.size;
            continue;
        }
        auto it = std::find_if(reasons.begin(), reasons.end(),
                               [&](const auto& r) { return r.first == image.reject; });
        if (it == reasons.end()) {
            reasons.emplace_back(image.reject, 1);
        } else {
            ++it->second;
        }
    }
    for (const auto& [why, count] : reasons) {
        rejected.push_back(fmt::format("{} GPU-modified images that cannot be flushed exactly ({})",
                                       count, why));
    }

    // Report.
    fmt::print("Replay capture inventory (frame end {}, inventory only, nothing written):\n",
               frames_seen);
    fmt::print("  G1 cut: {} after {:.1f} ms hold; {} submissions held ({:.1f} KiB copied at submit); "
               "renderer quiescent: {}; host commands during hold: {}\n",
               frame_ended ? "frame fully submitted" : "NOT REACHED", hold_ms, held.size(),
               snapshot_bytes / 1024.0, quiescent ? "yes" : ("no, " + quiesce_why), host_commands);
    fmt::print("  G2 commands: {} regions ({} constant, {} not copied), {} dwords, {} packets "
               "({} type-0, {} type-2), {} bad regions\n",
               census.regions, census.constant_regions, census.uncopied_regions, census.words,
               census.packets, census.type0, census.type2, census.bad_regions);
    std::string named, others;
    std::vector<std::pair<u64, u32>> by_count;
    for (u32 op = 0; op < 256; ++op) {
        if (census.opcodes[op]) by_count.emplace_back(census.opcodes[op], op);
    }
    std::sort(by_count.rbegin(), by_count.rend());
    for (const auto& [count, op] : by_count) {
        if (const char* name = OpcodeName(op)) {
            named += fmt::format(" {}={}", name, count);
        } else {
            others += fmt::format(" {:#04x}={}", op, count);
        }
    }
    fmt::print("  G2 opcodes:{}\n  G2 other opcodes:{}\n", named, others.empty() ? " none" : others);
    fmt::print("  G2 asynchronous compute submissions in window: {}\n", asc);
    if (memory_ok) {
        fmt::print("  G2 memory: {} direct views ({:.1f} MiB), {} flexible views ({:.1f} MiB), {} "
                   "reservations; unique backing {:.1f} MiB ({} views share backing); allocator: direct "
                   "live {:.1f} MiB, flexible in use {:.1f} MiB; low/untracked allocations not "
                   "counted\n",
                   memory.direct_vmas, memory.direct_view_bytes / 1048576.0, memory.flexible_vmas,
                   memory.flexible_view_bytes / 1048576.0, memory.reserved_vmas,
                   memory.unique_backing_bytes / 1048576.0, memory.shared_backing_views,
                   memory.live_direct_bytes / 1048576.0, memory.flexible_in_use_bytes / 1048576.0);
    } else {
        rejected.push_back("runtime memory inventory failed");
    }
    fmt::print("  G2 images: {} GPU-modified ({:.1f} MiB), flushable {:.1f} MiB\n", images.size(),
               image_bytes / 1048576.0, flushable_bytes / 1048576.0);
    for (const auto& image : images) {
        fmt::print("    image {:#x} {} bytes vk format {} tile {} aspects {:#x} samples {} mips {} "
                   "layers {}{}\n",
                   image.addr, image.size, image.pixel_format, image.tile_mode,
                   image.aspects, image.samples, image.mips, image.layers,
                   image.reject ? fmt::format(" REJECT ({})", image.reject) : std::string{});
    }
    if (rejected.empty()) {
        fmt::print("  VERDICT: cut admissible for v1\n");
    } else {
        fmt::print("  VERDICT: cut REJECTED:\n");
        for (const auto& why : rejected) {
            fmt::print("    - {}\n", why);
        }
    }
    std::fflush(stdout);
    release_time = std::chrono::steady_clock::now();
    state = State::Released;
}

void OnReleasedFrameRetired() {
    if (state.load() != State::Released) {
        return;
    }
    fmt::print("Replay capture: released frame retired {:.1f} ms after release; {} asynchronous "
               "compute submissions in the window; capture done\n",
               std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                         release_time).count(),
               asc_submits.load());
    std::fflush(stdout);
    state = State::Done;
}

} // namespace ReplayCapture

#else

namespace ReplayCapture {
bool Armed() {
    return false;
}
bool OnFrameRetired(Vulkan::Rasterizer*) {
    return false;
}
std::chrono::steady_clock::duration HoldRemaining() {
    return {};
}
void HoldEnded(Vulkan::Rasterizer*, bool, std::span<const std::shared_ptr<AmdGpu::CmdSnapshot>>, u32) {}
void NoteAscSubmit() {}
void OnReleasedFrameRetired() {}
} // namespace ReplayCapture

#endif
