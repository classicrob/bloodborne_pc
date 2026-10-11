// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: perf replay v0 loop (docs/macos/capture-replay.md "Perf replay v0"), Apple-only.
// bb-probe (BB_REPLAY_RUN) has mapped the captured guest memory; this replays the captured
// frame's submissions through the real HLE submit path N times and reports each iteration.
// Pixel output is not qualified; iteration 1 is warm-up.

#include <cstdint>

#ifdef __APPLE__

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <fmt/format.h>
#include <mach/mach.h>
#include "bbport_toggles.h"
#include "core/libraries/gnmdriver/gnmdriver.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/amdgpu/replay_format.h"
#include "video_core/amdgpu/replay_state_access.h"
#include "../../src/runtime_memory.h"

extern std::unique_ptr<AmdGpu::Liverpool> liverpool;

namespace {

using namespace ReplayFormat;

/// A capture file: header checked, records read in order, any short read is an error.
class Reader {
public:
    bool Open(const std::filesystem::path& path, const char (&magic)[8], std::string& why) {
        file = std::fopen(path.c_str(), "rb");
        name = path.string();
        if (!file) {
            why = fmt::format("cannot open {}", name);
            return false;
        }
        FileHeader header{};
        if (!Read(&header, sizeof(header), why)) {
            return false;
        }
        if (std::memcmp(header.magic, magic, 8) || header.version != Version) {
            why = fmt::format("{}: not a version {} capture file", name, Version);
            return false;
        }
        records = header.records;
        return true;
    }
    bool Read(void* out, size_t size, std::string& why) {
        if (size && std::fread(out, 1, size, file) != size) {
            why = fmt::format("{}: truncated", name);
            return false;
        }
        return true;
    }
    template <typename T>
    bool Get(T& value, std::string& why) {
        return Read(&value, sizeof(value), why);
    }
    ~Reader() {
        if (file) {
            std::fclose(file);
        }
    }
    u64 records = 0;

private:
    std::FILE* file = nullptr;
    std::string name;
};

struct Submission {
    const u32* dcb = nullptr;
    u32 dcb_bytes = 0;
    const u32* ccb = nullptr;
    u32 ccb_bytes = 0;
    struct Region {
        u64 guest;
        std::vector<u32> words;
    };
    std::vector<Region> regions;
};

struct Reset {
    u64 address;
    std::vector<u8> bytes;
};

struct Capture {
    std::unique_ptr<AmdGpu::Regs> regs = std::make_unique<AmdGpu::Regs>();
    u32 ce_counters[3]{};
    std::vector<u8> ce_heap;
    std::vector<std::pair<u32, AmdGpu::ComputeProgram>> cs_states;
    VAddr indirect_args = 0;
    std::vector<Submission> submissions;
    std::vector<Reset> resets;

    bool Load(const std::filesystem::path& dir, std::string& why) {
        return LoadLiverpool(dir, why) && LoadSubmissions(dir, why) && LoadResets(dir, why);
    }

private:
    bool LoadLiverpool(const std::filesystem::path& dir, std::string& why) {
        Reader r;
        if (!r.Open(dir / "liverpool.bin", LiverpoolMagic, why)) {
            return false;
        }
        bool have_regs = false, have_heap = false;
        for (u64 i = 0; i < r.records; ++i) {
            SectionRecord s{};
            if (!r.Get(s, why)) {
                return false;
            }
            const auto expect = [&](u64 size) {
                if (s.bytes != size) {
                    why = fmt::format("liverpool.bin section {} is {} bytes, this build expects {} "
                                      "(capture from another build)",
                                      u32(s.section), s.bytes, size);
                    return false;
                }
                return true;
            };
            switch (s.section) {
            case LiverpoolSection::Regs:
                if (!expect(sizeof(*regs)) || !r.Read(regs.get(), sizeof(*regs), why)) {
                    return false;
                }
                have_regs = true;
                break;
            case LiverpoolSection::CeCounters:
                if (!expect(sizeof(ce_counters)) || !r.Read(ce_counters, sizeof(ce_counters), why)) {
                    return false;
                }
                break;
            case LiverpoolSection::CeHeap:
                if (!expect(AmdGpu::ReplayStateAccess::CeHeap().size())) {
                    return false;
                }
                ce_heap.resize(s.bytes);
                if (!r.Read(ce_heap.data(), ce_heap.size(), why)) {
                    return false;
                }
                have_heap = true;
                break;
            case LiverpoolSection::QueueCsState: {
                struct {
                    u32 queue;
                    AmdGpu::ComputeProgram cs;
                } entry{};
                if (!expect(sizeof(entry)) || !r.Read(&entry, sizeof(entry), why)) {
                    return false;
                }
                if (entry.queue >= AmdGpu::Liverpool::NumTotalQueues) {
                    why = fmt::format("liverpool.bin: queue {} out of range", entry.queue);
                    return false;
                }
                cs_states.emplace_back(entry.queue, entry.cs);
                break;
            }
            case LiverpoolSection::IndirectArgs:
                if (!expect(sizeof(indirect_args)) ||
                    !r.Read(&indirect_args, sizeof(indirect_args), why)) {
                    return false;
                }
                break;
            default:
                why = fmt::format("liverpool.bin: unknown section {}", u32(s.section));
                return false;
            }
        }
        if (!have_regs || !have_heap) {
            why = "liverpool.bin lacks the registers or the CE heap";
            return false;
        }
        return true;
    }

    bool LoadSubmissions(const std::filesystem::path& dir, std::string& why) {
        Reader r;
        if (!r.Open(dir / "submissions.bin", SubmissionsMagic, why)) {
            return false;
        }
        for (u64 i = 0; i < r.records; ++i) {
            SubmissionRecord s{};
            if (!r.Get(s, why)) {
                return false;
            }
            if (s.dcb_region < 0 || u32(s.dcb_region) >= s.regions ||
                (s.ccb_region >= 0 && u32(s.ccb_region) >= s.regions)) {
                why = fmt::format("submissions.bin: submission {} has invalid region indices", i);
                return false;
            }
            Submission sub;
            for (u32 k = 0; k < s.regions; ++k) {
                RegionRecord rr{};
                if (!r.Get(rr, why)) {
                    return false;
                }
                if (!rr.copied) {
                    why = fmt::format("submission {} region {:#x} was not copied at capture", i,
                                      rr.guest);
                    return false;
                }
                Submission::Region region{rr.guest, std::vector<u32>(rr.size_dw)};
                if (!r.Read(region.words.data(), region.words.size() * 4, why)) {
                    return false;
                }
                if (s32(k) == s.dcb_region) {
                    sub.dcb = reinterpret_cast<const u32*>(rr.guest);
                    sub.dcb_bytes = rr.size_dw * 4;
                }
                if (s32(k) == s.ccb_region) {
                    sub.ccb = reinterpret_cast<const u32*>(rr.guest);
                    sub.ccb_bytes = rr.size_dw * 4;
                }
                sub.regions.push_back(std::move(region));
            }
            submissions.push_back(std::move(sub));
        }
        if (submissions.empty()) {
            why = "submissions.bin holds no submission";
            return false;
        }
        return true;
    }

    bool LoadResets(const std::filesystem::path& dir, std::string& why) {
        Reader r;
        if (!r.Open(dir / "resets.bin", ResetsMagic, why)) {
            return false;
        }
        for (u64 i = 0; i < r.records; ++i) {
            ResetRecord rr{};
            if (!r.Get(rr, why)) {
                return false;
            }
            Reset reset{rr.address, std::vector<u8>(rr.bytes)};
            if (!r.Read(reset.bytes.data(), reset.bytes.size(), why)) {
                return false;
            }
            resets.push_back(std::move(reset));
        }
        return true;
    }
};

bool WaitIdle(AmdGpu::Liverpool& lp, std::chrono::seconds bound) {
    const auto end = std::chrono::steady_clock::now() + bound;
    while (!lp.IsGpuIdle()) {
        if (std::chrono::steady_clock::now() > end) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    return true;
}

/// CPU time of the GPU command thread (user + system), in microseconds.
u64 GpuThreadCpuUs() {
    const mach_port_t port = BbStats::gpu_thread_port.load();
    thread_basic_info_data_t info{};
    mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
    if (!port ||
        thread_info(port, THREAD_BASIC_INFO, reinterpret_cast<thread_info_t>(&info), &count) !=
            KERN_SUCCESS) {
        return 0;
    }
    return u64(info.user_time.seconds + info.system_time.seconds) * 1000000 +
           info.user_time.microseconds + info.system_time.microseconds;
}

struct Counters {
    u64 gpu_cpu_us, idle_ns, draws, dispatches, submissions, images, image_bytes, buffer_bytes;
    static Counters Now() {
        return {GpuThreadCpuUs(),
                BbStats::gpu_idle_ns.load(),
                BbStats::draws.load(),
                BbStats::dispatches.load(),
                BbStats::submissions.load(),
                BbStats::images_registered.load(),
                BbStats::image_upload_bytes.load(),
                BbStats::buffer_upload_bytes.load()};
    }
};

/// Restores everything the frame starts from: the reset ranges, the command words and the
/// Liverpool parse state. The GPU command thread is idle (waiting for a submission).
bool Restore(AmdGpu::Liverpool& lp, const Capture& c, std::string& why) {
    for (const auto& reset : c.resets) {
        if (!runtime_memory_write_backing(reset.address, reset.bytes.data(), reset.bytes.size())) {
            why = fmt::format("reset range {:#x} (+{}) is not mapped", reset.address,
                              reset.bytes.size());
            return false;
        }
    }
    for (const auto& sub : c.submissions) {
        for (const auto& region : sub.regions) {
            if (!runtime_memory_write_backing(region.guest, region.words.data(),
                                              region.words.size() * 4)) {
                why = fmt::format("command region {:#x} is not mapped", region.guest);
                return false;
            }
        }
    }
    std::memcpy(&lp.regs, c.regs.get(), sizeof(lp.regs));
    std::memcpy(AmdGpu::ReplayStateAccess::CeHeap().data(), c.ce_heap.data(), c.ce_heap.size());
    AmdGpu::ReplayStateAccess::SetCeCounters(lp, c.ce_counters[0], c.ce_counters[1],
                                             c.ce_counters[2]);
    for (const auto& [queue, cs] : c.cs_states) {
        AmdGpu::ReplayStateAccess::SetCsState(lp, queue, cs);
    }
    AmdGpu::ReplayStateAccess::SetIndirectArgs(lp, c.indirect_args);
    return true;
}

} // namespace

/// Replays the capture in `dir` `iterations` times; 0 when every iteration ran, else nonzero
/// after a STOP line.
extern "C" int bbgpu_replay_run(const char* dir, unsigned iterations) {
    std::string why;
    Capture capture;
    if (!capture.Load(dir, why)) {
        std::fprintf(stderr, "STOP: perf replay: %s\n", why.c_str());
        return 2;
    }
    AmdGpu::Liverpool* lp = liverpool.get();
    if (!lp) {
        std::fprintf(stderr, "STOP: perf replay: the GPU library has no Liverpool\n");
        return 2;
    }
    // A Parse split block per iteration. No frame has ended in this process yet, so the window
    // is still unread.
    const char* window = std::getenv("BB_SPLIT_WINDOW");
    if (!window) {
        setenv("BB_SPLIT_WINDOW", "1", 0);
    } else if (std::strcmp(window, "1")) {
        std::fprintf(stderr, "STOP: perf replay: BB_SPLIT_WINDOW=%s; the replay reports per "
                             "iteration (1)\n", window);
        return 2;
    }
    fmt::print("Perf replay: {} ({} submissions, {} reset ranges), {} iterations; pixel output not "
               "qualified, iteration 1 is warm-up\n",
               dir, capture.submissions.size(), capture.resets.size(), iterations);
    std::fflush(stdout);
    std::vector<double> wall_ms, cpu_ms;
    for (unsigned it = 1; it <= iterations; ++it) {
        if (!WaitIdle(*lp, std::chrono::seconds(30))) {
            std::fprintf(stderr, "STOP: perf replay: the GPU did not go idle within 30 s before "
                                 "iteration %u\n", it);
            return 3;
        }
        if (!Restore(*lp, capture, why)) {
            std::fprintf(stderr, "STOP: perf replay: %s\n", why.c_str());
            return 3;
        }
        const Counters before = Counters::Now();
        const auto t0 = std::chrono::steady_clock::now();
        for (const auto& sub : capture.submissions) {
            const u32* dcb[1] = {sub.dcb};
            u32 dcb_bytes[1] = {sub.dcb_bytes};
            const u32* ccb[1] = {sub.ccb};
            u32 ccb_bytes[1] = {sub.ccb_bytes};
            const s32 result = Libraries::GnmDriver::sceGnmSubmitCommandBuffers(
                1, dcb, dcb_bytes, sub.ccb ? ccb : nullptr, sub.ccb ? ccb_bytes : nullptr);
            if (result != 0) {
                std::fprintf(stderr, "STOP: perf replay: sceGnmSubmitCommandBuffers failed "
                                     "(%#x) in iteration %u\n", unsigned(result), it);
                return 3;
            }
        }
        Libraries::GnmDriver::sceGnmSubmitDone();
        if (!WaitIdle(*lp, std::chrono::seconds(30))) {
            std::fprintf(stderr, "STOP: perf replay: iteration %u did not retire within 30 s (a "
                                 "wait the reset list does not satisfy?)\n", it);
            return 3;
        }
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        const Counters after = Counters::Now();
        const double gpu_cpu = (after.gpu_cpu_us - before.gpu_cpu_us) / 1000.0;
        wall_ms.push_back(ms);
        cpu_ms.push_back(gpu_cpu);
        fmt::print("Perf replay iteration {}{}: {:.2f} ms submit-to-idle, GPU thread CPU {:.2f} ms "
                   "(idle {:.2f} ms); {} draws, {} dispatches, {} submissions; images registered "
                   "{}, image uploads {:.1f} MiB, buffer uploads {:.1f} MiB\n",
                   it, it == 1 ? " (warm-up)" : "", ms, gpu_cpu,
                   (after.idle_ns - before.idle_ns) / 1e6, after.draws - before.draws,
                   after.dispatches - before.dispatches, after.submissions - before.submissions,
                   after.images - before.images,
                   (after.image_bytes - before.image_bytes) / 1048576.0,
                   (after.buffer_bytes - before.buffer_bytes) / 1048576.0);
        std::fflush(stdout);
    }
    if (iterations >= 3) {
        auto median = [](std::vector<double> v) {
            std::sort(v.begin(), v.end());
            return v[v.size() / 2];
        };
        std::vector<double> w(wall_ms.begin() + 1, wall_ms.end()), c(cpu_ms.begin() + 1, cpu_ms.end());
        fmt::print("Perf replay steady state (iterations 2..{}): submit-to-idle median {:.2f} ms "
                   "[{:.2f}, {:.2f}], GPU thread CPU median {:.2f} ms [{:.2f}, {:.2f}]\n",
                   iterations, median(w), *std::min_element(w.begin(), w.end()),
                   *std::max_element(w.begin(), w.end()), median(c),
                   *std::min_element(c.begin(), c.end()), *std::max_element(c.begin(), c.end()));
    }
    std::fflush(stdout);
    return 0;
}

#else

extern "C" int bbgpu_replay_run(const char*, unsigned) {
    return 2;
}

#endif
