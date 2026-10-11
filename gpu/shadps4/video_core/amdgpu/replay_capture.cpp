// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/amdgpu/replay_capture.h"

namespace ReplayCapture::Detail {

std::optional<u64> ParseBounded(std::string_view text, u64 max) {
    if (text.empty()) {
        return std::nullopt;
    }
    u64 value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        const u64 digit = static_cast<u64>(c - '0');
        if (digit > max || value > (max - digit) / 10) {
            return std::nullopt; // above max (and so never wraps)
        }
        value = value * 10 + digit;
    }
    return value;
}

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
            ++census.bad_header_regions;
            return;
        }
        const u64 length = u64{(header >> 16) & 0x3FFF} + 2; // header + count + 1 payload dwords
        if (length > words.size() - i) {
            ++census.truncated_regions;
            return;
        }
        if (type == 3) {
            ++census.opcodes[(header >> 8) & 0xFF];
            ++census.packets;
        } else {
            ++census.type0;
        }
        i += length;
    }
}

} // namespace ReplayCapture::Detail

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

using Detail::ParseBounded;
using Detail::Pm4Census;

enum class State { Off, Waiting, Holding, Released, Done };

struct Config {
    u64 arm_frames = 0;
    std::chrono::milliseconds hold_bound{3000};
    bool valid = false;
};

constexpr u64 MaxArmFrames = 100000;
constexpr u64 MaxHoldMs = 60000;

const Config& Settings() {
    static const Config config = [] {
        Config c;
        const char* value = std::getenv("BB_REPLAY_CAPTURE");
        if (!value || !*value) {
            return c;
        }
        const std::string_view text{value};
        constexpr std::string_view prefix = "inventory:";
        if (!text.starts_with(prefix)) {
            std::fprintf(stderr, "*** BB_REPLAY_CAPTURE=%s: only inventory:<frames> is implemented; "
                                 "capture disabled ***\n", value);
            return c;
        }
        const auto frames = ParseBounded(text.substr(prefix.size()), MaxArmFrames);
        if (!frames || *frames == 0) {
            std::fprintf(stderr, "*** BB_REPLAY_CAPTURE=%s: <frames> must be decimal digits, 1..%llu; "
                                 "capture disabled ***\n", value,
                         static_cast<unsigned long long>(MaxArmFrames));
            return c;
        }
        c.arm_frames = *frames;
        if (const char* ms = std::getenv("BB_REPLAY_HOLD_MS")) {
            const auto bound = ParseBounded(ms, MaxHoldMs);
            if (!bound || *bound == 0) {
                std::fprintf(stderr, "*** BB_REPLAY_HOLD_MS=%s: must be decimal digits, 1..%llu; "
                                     "capture disabled ***\n", ms,
                             static_cast<unsigned long long>(MaxHoldMs));
                return c;
            }
            c.hold_bound = std::chrono::milliseconds(static_cast<std::chrono::milliseconds::rep>(*bound));
        }
        c.valid = true;
        std::fprintf(stderr, "\n*** BB_REPLAY_CAPTURE: inventory at retired frame end %llu, hold bound "
                             "%lld ms (diagnostic: holds the GPU command thread for one frame) ***\n\n",
                     static_cast<unsigned long long>(c.arm_frames),
                     static_cast<long long>(c.hold_bound.count()));
        return c;
    }();
    return config;
}

std::atomic<State> state{State::Off};
std::atomic<u64> asc_submits{0}; // monotonic while a capture is in progress
u64 asc_at_checkpoint = 0;
u64 frames_seen = 0;
std::chrono::steady_clock::time_point hold_start, release_time;

/// PM4 type-3 opcodes that matter to a replay cut (capture-replay.md "Dependencies without a
/// guest CPU"), and the rest by count.
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
    if (s == State::Waiting || s == State::Holding || s == State::Released) {
        asc_submits.fetch_add(1, std::memory_order_relaxed);
    }
}

u64 AscSubmitted() {
    return asc_submits.load();
}

void HoldEnded(Vulkan::Rasterizer* rasterizer, const Held& held) {
    const double hold_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - hold_start).count();
    std::vector<std::string> rejected;

    // G1: the cut.
    if (held.ended_early) {
        rejected.push_back(fmt::format("the hold ended early: {}", held.ended_early));
    } else if (!held.frame_ended) {
        rejected.push_back(fmt::format(
            "the guest did not end the held frame within {} ms ({} submissions held): the cut was "
            "not reached",
            Settings().hold_bound.count(), held.gfx.size()));
    }
    std::string quiesce_why;
    const bool quiescent = rasterizer->QuiesceForCapture(quiesce_why);
    if (!quiescent) {
        rejected.push_back("renderer not quiescent at the cut: " + quiesce_why);
    }
    if (held.host_commands) {
        rejected.push_back(fmt::format("{} host commands ran during the hold (memory mapping or "
                                       "cache operations inside the cut window)", held.host_commands));
    }
    if (held.gfx_tasks != held.gfx.size()) {
        rejected.push_back(fmt::format("{} graphics tasks queued but {} unparsed graphics snapshots: the "
                                       "held set is not the queued work", held.gfx_tasks,
                                       held.gfx.size()));
    }

    // G2: the held frame's command stream.
    Pm4Census census;
    u64 snapshot_bytes = 0;
    for (const auto& snapshot : held.gfx) {
        snapshot_bytes += snapshot->copied_bytes;
        for (const auto& region : snapshot->Regions()) {
            ++census.regions;
            census.constant_regions += region.constant;
            if (!region.copied) {
                ++census.uncopied_regions;
                continue;
            }
            census.words += region.size_dw;
            Detail::CountRegion(census, snapshot->Words(region));
        }
    }
    if (!census.Complete()) {
        rejected.push_back(fmt::format(
            "command census incomplete ({} regions not copied, {} stopped at an invalid header, {} at "
            "a truncated packet): the opcode counts cover only the scanned part",
            census.uncopied_regions, census.bad_header_regions, census.truncated_regions));
    }
    // ASC: queued at sampling, plus any counted from then to this report checkpoint (NoteAscSubmit
    // runs after the enqueue and asc_seen was read before sampling, so none is missed; one may be
    // counted in both). Later ones, including report-to-release time, are reported at closure.
    asc_at_checkpoint = asc_submits.load();
    const u64 asc_to_checkpoint = asc_at_checkpoint - held.asc_seen;
    if (held.asc_tasks || asc_to_checkpoint) {
        rejected.push_back(fmt::format("asynchronous-compute submissions in the window: {} queued at "
                                       "sampling, {} counted from sampling to the report "
                                       "checkpoint (v1 has no ASC capture)", held.asc_tasks, asc_to_checkpoint));
    }
    if (census.opcodes[0x59]) {
        rejected.push_back(fmt::format("{} potential REWIND packets (v1 rejects live command "
                                       "refetch)", census.opcodes[0x59]));
    }

    // G2: memory and image authority.
    RuntimeMemoryInventory memory{};
    const bool memory_ok = runtime_memory_inventory(&memory) == 0;
    std::vector<VideoCore::CaptureImageRecord> images;
    rasterizer->GetTextureCache().ReportGpuModifiedForCapture(images);
    u64 image_bytes = 0, eligible_bytes = 0;
    std::vector<std::pair<std::string, u32>> reasons;
    for (const auto& image : images) {
        image_bytes += image.size;
        if (!image.reject) {
            eligible_bytes += image.size;
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
        rejected.push_back(fmt::format("{} GPU-modified images not eligible for writeback ({})",
                                       count, why));
    }

    // Report.
    fmt::print("Replay capture inventory (frame end {}, inventory only, nothing written):\n",
               frames_seen);
    fmt::print("  G1 cut: {} after {:.1f} ms hold; {} graphics submissions held ({:.1f} KiB copied at "
               "submit, {} graphics tasks queued); renderer quiescent: {}; host commands during "
               "hold: {}\n",
               held.ended_early ? "ENDED EARLY"
               : held.frame_ended ? "frame fully submitted"
                                  : "NOT REACHED",
               hold_ms, held.gfx.size(), snapshot_bytes / 1024.0, held.gfx_tasks,
               quiescent ? "yes" : ("no, " + quiesce_why), held.host_commands);
    fmt::print("  G2 commands (raw scan, potential packets): {} regions ({} constant, {} not copied), "
               "{} dwords, {} type-3 ({} type-0, {} type-2); scan stopped early: {} invalid header, "
               "{} truncated\n",
               census.regions, census.constant_regions, census.uncopied_regions, census.words,
               census.packets, census.type0, census.type2, census.bad_header_regions,
               census.truncated_regions);
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
    fmt::print("  G2 asynchronous compute submissions: {} queued at sampling, {} counted from "
               "sampling to this report (may overlap)\n", held.asc_tasks, asc_to_checkpoint);
    if (memory_ok) {
        fmt::print("  G2 memory: {} direct views ({:.1f} MiB), {} flexible views ({:.1f} MiB), {} "
                   "reservations; unique backing {:.1f} MiB ({} views share backing); allocator: "
                   "direct live {:.1f} MiB, flexible in use {:.1f} MiB; low/untracked allocations "
                   "not counted\n",
                   memory.direct_vmas, memory.direct_view_bytes / 1048576.0, memory.flexible_vmas,
                   memory.flexible_view_bytes / 1048576.0, memory.reserved_vmas,
                   memory.unique_backing_bytes / 1048576.0, memory.shared_backing_views,
                   memory.live_direct_bytes / 1048576.0, memory.flexible_in_use_bytes / 1048576.0);
    } else {
        rejected.push_back("runtime memory inventory failed");
    }
    fmt::print("  G2 images: {} GPU-modified ({:.1f} MiB), writeback-eligible {:.1f} MiB (eligible "
               "only: writeback not exercised)\n",
               images.size(), image_bytes / 1048576.0, eligible_bytes / 1048576.0);
    for (const auto& image : images) {
        fmt::print("    image {:#x} {} bytes vk format {} tile {} aspects {:#x} samples {} mips {} "
                   "layers {}{}\n",
                   image.addr, image.size, image.pixel_format, image.tile_mode,
                   image.aspects, image.samples, image.mips, image.layers,
                   image.reject ? fmt::format(" REJECT ({})", image.reject) : std::string{});
    }
    if (rejected.empty()) {
        fmt::print("  VERDICT: no inventoried blocker found; capture NOT yet qualified. Unproven here: "
                   "guest CPU and HLE writers are not rendezvoused at the cut; low/untracked host "
                   "memory is omitted; images without a reject are writeback-eligible only; the "
                   "command census counts potential, not executed, packets; asynchronous compute "
                   "submitted after this report is reported when the window closes\n");
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
    const u64 asc = asc_submits.load() - asc_at_checkpoint;
    fmt::print("Replay capture: released frame retired {:.1f} ms after release; {} asynchronous "
               "compute submissions between the report checkpoint (before release) and "
               "retirement{}; capture done\n",
               std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                         release_time).count(),
               asc, asc ? " (v1 cannot tell whether the held frame depends on them: NOT qualified)"
                        : "");
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
void HoldEnded(Vulkan::Rasterizer*, const Held&) {}
void NoteAscSubmit() {}
u64 AscSubmitted() {
    return 0;
}
void OnReleasedFrameRetired() {}
} // namespace ReplayCapture

#endif
