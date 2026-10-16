// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: perf replay v0 loop (docs/macos/capture-replay.md "Perf replay v0"), Apple-only.
// bb-probe (BB_REPLAY_RUN) has mapped the captured guest memory; this replays the captured
// frame's submissions through the real HLE submit path N times and reports each iteration.
// Pixel output is not qualified; iteration 1 is warm-up.

#include <cerrno>
#include <cstdint>
#include <atomic>
#include "bbport_replay_active.h"

namespace {
std::atomic<bool> measurement_active{false};
}
namespace BbReplay {
bool MeasurementActive() { return measurement_active.load(std::memory_order_acquire); }
}

#ifdef __APPLE__

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <fmt/format.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <libproc.h>
#include <mach/task_policy.h>
#include <sys/resource.h>
#include <unistd.h>
#include "bbport_toggles.h"
#include "macos_perf.h"
#include "core/libraries/gnmdriver/gnmdriver.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/amdgpu/replay_format.h"
#include "video_core/amdgpu/replay_state_access.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/texture_cache/capture_images.h"
#include <map>
#include <xxhash.h>
#include "../../src/runtime_memory.h"
#include "bbport_replay_compare.h"

extern std::unique_ptr<AmdGpu::Liverpool> liverpool;

namespace {

using namespace ReplayFormat;

/// A capture file: header checked, records read in order, any short read is an error.
class Reader {
public:
    bool Open(const std::filesystem::path& path, const char (&magic)[8], std::string& why,
              u32 version = Version) {
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
        if (std::memcmp(header.magic, magic, 8) || header.version != version || header.reserved) {
            why = fmt::format("{}: expected version {} capture file (legacy host resets need explicit migration)", name, version);
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
    bool End(std::string& why) {
        if (std::fgetc(file) != EOF || std::ferror(file)) {
            why = fmt::format("{}: trailing bytes or read error", name);
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
        bool top_level = false; ///< the submission's DCB or CCB (its address is in no packet)
        bool from_copy = false; ///< outside guest memory here: submitted from `words`
    };
    std::vector<Region> regions;
    s32 dcb_region = -1, ccb_region = -1;
};

struct Reset {
    u64 address;
    std::vector<u8> bytes;
    bool host; ///< outside the guest views: restored only into the identified object
    HostObjectRecord object{};
};

struct Capture {
    std::unique_ptr<AmdGpu::Regs> regs = std::make_unique<AmdGpu::Regs>();
    u32 ce_counters[3]{};
    std::vector<u8> ce_heap;
    std::vector<std::pair<u32, AmdGpu::ComputeProgram>> cs_states;
    VAddr indirect_args = 0;
    u64 gfx_reg_checksum = 0;
    bool have_checksum = false;
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
            case LiverpoolSection::GfxRegChecksum:
                if (!expect(sizeof(gfx_reg_checksum)) ||
                    !r.Read(&gfx_reg_checksum, sizeof(gfx_reg_checksum), why)) {
                    return false;
                }
                have_checksum = true;
                break;
            default:
                why = fmt::format("liverpool.bin: unknown section {}", u32(s.section));
                return false;
            }
        }
        if (!have_regs || !have_heap || !have_checksum) {
            why = "liverpool.bin lacks the registers, the CE heap or the register checksum";
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
                region.top_level = s32(k) == s.dcb_region || s32(k) == s.ccb_region;
                sub.regions.push_back(std::move(region));
            }
            sub.dcb_region = s.dcb_region;
            sub.ccb_region = s.ccb_region;
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
        if (!r.Open(dir / "resets.bin", ResetsMagic, why, ResetsVersion)) {
            return false;
        }
        if (r.records > MaxResetRecords) {
            why = "resets.bin: more than 1000000 ranges";
            return false;
        }
        u64 previous_end = 0;
        for (u64 i = 0; i < r.records; ++i) {
            ResetRecord rr{};
            if (!r.Get(rr, why)) {
                return false;
            }
            if (rr.host > 1 || !rr.bytes || rr.bytes > MaxResetBytes ||
                rr.address > UINT64_MAX - rr.bytes || rr.kind < ResetKind::Written ||
                rr.kind > ResetKind::Indirect || rr.address < previous_end) {
                why = "resets.bin: invalid host flag, kind, size or wrapping span";
                return false;
            }
            previous_end = rr.address + rr.bytes;
            Reset reset{rr.address, std::vector<u8>(rr.bytes), rr.host != 0};
            if (!r.Read(reset.bytes.data(), reset.bytes.size(), why) ||
                (reset.host && !r.Get(reset.object, why))) {
                return false;
            }
            if (reset.host && !BbReplay::HostSpanValid(reset.object, reset.address,
                                                       reset.bytes.size())) {
                why = "resets.bin: invalid named host range or reset offset";
                return false;
            }
            resets.push_back(std::move(reset));
        }
        return r.End(why);
    }
};

bool WaitIdle(AmdGpu::Liverpool& lp, std::chrono::seconds bound);

/// Before any CPU write of guest bytes: the command processor idle (WaitIdle), then on the GPU
/// command thread everything recorded submitted and complete on the GPU and every deferred write
/// (labels, downloads) run (Rasterizer::QuiesceForCapture). IsGpuIdle alone is not a fence for
/// earlier GPU work, which may still read or write the bytes the restore rewrites.
bool QuiesceForRestore(AmdGpu::Liverpool& lp, std::string& why) {
    if (!WaitIdle(lp, std::chrono::seconds(30))) {
        why = "the command processor did not go idle within 30 s";
        return false;
    }
    Vulkan::Rasterizer* rasterizer = AmdGpu::ReplayStateAccess::Rasterizer(lp);
    if (!rasterizer) {
        why = "no rasterizer";
        return false;
    }
    std::string quiet_why;
    bool ok = false;
    lp.SendCommand<true>([&] { ok = rasterizer->QuiesceForCapture(quiet_why); });
    if (!ok) {
        why = "the GPU work before the restore did not complete: " + quiet_why;
    }
    return ok;
}

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

/// Process and host memory counters, read around each iteration to find what changes when the
/// replay loop slows down: CPU time split user/system, page faults, context switches, the
/// process's physical footprint and the host's paging/compressor activity.
struct ProcessCounters {
    u64 user_us = 0, system_us = 0, minor_faults = 0, major_faults = 0, voluntary_switches = 0,
        involuntary_switches = 0, footprint = 0, pageins = 0, pageouts = 0, compressions = 0,
        decompressions = 0, swapins = 0, swapouts = 0;
    // proc_pid_rusage v6 (kernel time units; printed as ratios, which cancel them).
    u64 ri_time = 0, ri_ptime = 0, ri_runnable = 0, instructions = 0, cycles = 0, energy_nj = 0;
    u64 qos[7] = {}; // default, maintenance, background, utility, legacy, user initiated, user
                     // interactive
    int role = 0; // task_role_t of TASK_CATEGORY_POLICY
    static bool Now(ProcessCounters& c, std::string& why) {
        rusage usage{};
        if (getrusage(RUSAGE_SELF, &usage) != 0) {
            why = fmt::format("getrusage failed (errno {})", errno);
            return false;
        }
        c.user_us = u64(usage.ru_utime.tv_sec) * 1000000 + u64(usage.ru_utime.tv_usec);
        c.system_us = u64(usage.ru_stime.tv_sec) * 1000000 + u64(usage.ru_stime.tv_usec);
        c.minor_faults = u64(usage.ru_minflt);
        c.major_faults = u64(usage.ru_majflt);
        c.voluntary_switches = u64(usage.ru_nvcsw);
        c.involuntary_switches = u64(usage.ru_nivcsw);
        task_vm_info_data_t vm{};
        mach_msg_type_number_t vm_count = TASK_VM_INFO_COUNT;
        if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&vm),
                      &vm_count) != KERN_SUCCESS) {
            why = "task_info(TASK_VM_INFO) failed";
            return false;
        }
        c.footprint = vm.phys_footprint;
        vm_statistics64_data_t host{};
        mach_msg_type_number_t host_count = HOST_VM_INFO64_COUNT;
        mach_port_t self = mach_host_self();
        const kern_return_t kr = host_statistics64(
            self, HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&host), &host_count);
        mach_port_deallocate(mach_task_self(), self);
        if (kr != KERN_SUCCESS) {
            why = "host_statistics64(HOST_VM_INFO64) failed";
            return false;
        }
        c.pageins = host.pageins;
        c.pageouts = host.pageouts;
        c.compressions = host.compressions;
        c.decompressions = host.decompressions;
        c.swapins = host.swapins;
        c.swapouts = host.swapouts;
        rusage_info_v6 ri{};
        if (proc_pid_rusage(getpid(), RUSAGE_INFO_V6, reinterpret_cast<rusage_info_t*>(&ri)) !=
            0) {
            why = fmt::format("proc_pid_rusage(RUSAGE_INFO_V6) failed (errno {})", errno);
            return false;
        }
        c.ri_time = ri.ri_user_time + ri.ri_system_time;
        c.ri_ptime = ri.ri_user_ptime + ri.ri_system_ptime;
        c.ri_runnable = ri.ri_runnable_time;
        c.instructions = ri.ri_instructions;
        c.cycles = ri.ri_cycles;
        c.energy_nj = ri.ri_energy_nj;
        const u64 qos[7] = {ri.ri_cpu_time_qos_default,     ri.ri_cpu_time_qos_maintenance,
                            ri.ri_cpu_time_qos_background,  ri.ri_cpu_time_qos_utility,
                            ri.ri_cpu_time_qos_legacy,      ri.ri_cpu_time_qos_user_initiated,
                            ri.ri_cpu_time_qos_user_interactive};
        std::copy(std::begin(qos), std::end(qos), c.qos);
        task_category_policy_data_t category{};
        mach_msg_type_number_t category_count = TASK_CATEGORY_POLICY_COUNT;
        boolean_t get_default = FALSE;
        if (task_policy_get(mach_task_self(), TASK_CATEGORY_POLICY,
                            reinterpret_cast<task_policy_t>(&category), &category_count,
                            &get_default) != KERN_SUCCESS) {
            why = "task_policy_get(TASK_CATEGORY_POLICY) failed";
            return false;
        }
        c.role = category.role;
        return true;
    }
};

/// Restores everything the frame starts from: the reset ranges, the command words and the
/// Liverpool parse state. The GPU command thread is idle (waiting for a submission).
bool Restore(AmdGpu::Liverpool& lp, Capture& c, std::string& why) {
    // Validate every host identity before writing even the first guest or host reset.
    for (const auto& reset : c.resets) {
        if (!reset.host) {
            for (u64 at = reset.address, end = at + reset.bytes.size(); at < end;) {
                uintptr_t lo = 0, hi = 0;
                int mapped = 0;
                if (!runtime_memory_region(at, &lo, &hi, &mapped) || !mapped || hi <= at) {
                    why = fmt::format("reset range {:#x} (+{}) is not fully mapped",
                                      reset.address, reset.bytes.size());
                    return false;
                }
                at = std::min<u64>(hi, end);
            }
            continue;
        }
        const auto& want = reset.object;
        RuntimeReplayObject live{};
        const bool found = runtime_replay_named(want.name, &live) == 0;
        if (!found || !BbReplay::HostMatches(want, reset.address, reset.bytes.size(),
                                            live.start, live.size)) {
            why = fmt::format("host reset range {:#x} (+{}): named object '{}' expected "
                              "[{:#x}, +{:#x}) offset {:#x}; here {}",
                              reset.address, reset.bytes.size(), want.name, want.start, want.size,
                              want.offset, found ? fmt::format("[{:#x}, +{:#x})", live.start,
                                                              live.size)
                                                 : std::string{"name not registered"});
            return false;
        }
    }
    for (const auto& reset : c.resets) {
        if (reset.host) {
            std::memcpy(reinterpret_cast<void*>(reset.address), reset.bytes.data(),
                        reset.bytes.size());
            continue;
        }
        if (!runtime_memory_write_backing(reset.address, reset.bytes.data(), reset.bytes.size())) {
            why = fmt::format("reset range {:#x} (+{}) is not mapped", reset.address,
                              reset.bytes.size());
            return false;
        }
    }
    for (auto& sub : c.submissions) {
        for (size_t k = 0; k < sub.regions.size(); ++k) {
            auto& region = sub.regions[k];
            if (region.from_copy ||
                runtime_memory_write_backing(region.guest, region.words.data(),
                                             region.words.size() * 4)) {
                continue;
            }
            if (!region.top_level) {
                // An indirect buffer's address is inside a packet: it must be where it was.
                why = fmt::format("indirect buffer {:#x} ({} dwords) lies outside guest memory "
                                  "here (an HLE-built buffer in host memory?)",
                                  region.guest, region.words.size());
                return false;
            }
            // A DCB/CCB in host memory (an HLE-built buffer): only the submit call holds its
            // address, so it is submitted from the capture's copy.
            region.from_copy = true;
            const u32* copy = region.words.data();
            if (s32(k) == sub.dcb_region) {
                sub.dcb = copy;
            } else {
                sub.ccb = copy;
            }
            fmt::print("Perf replay: {} {:#x} ({} dwords) lies outside guest memory here; submitted "
                       "from the capture's copy\n",
                       s32(k) == sub.dcb_region ? "DCB" : "CCB", region.guest,
                       region.words.size());
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
    lp.gfx_reg_checksum = c.gfx_reg_checksum;
    // Draw preparation restarts from the restored registers (vk_draw_prep.h ResetBaseline).
    if (Vulkan::Rasterizer* rasterizer = AmdGpu::ReplayStateAccess::Rasterizer(lp)) {
        auto& prep = rasterizer->GetDrawPreparation();
        if (prep.Enabled()) {
            prep.ResetBaseline(lp.gfx_submit_seq, lp.regs, lp.gfx_reg_checksum);
        }
    }
    return true;
}

/// Runs `step` on the GPU command thread (a blocking host command): the restore steps and the
/// image capture/restore are GPU-thread work.
bool OnGpuThread(AmdGpu::Liverpool& lp, const std::function<bool()>& step) {
    bool ok = false;
    lp.SendCommand<true>([&] { ok = step(); });
    return ok;
}

/// What identifies a captured image and what its bytes are, for the round-trip comparison.
struct ImagePrint {
    VideoCore::CapturedImageState state{};
    std::vector<std::pair<std::string, u64>> aspects; ///< "aspect a mip m", XXH3 of the texels
};
using ImageKey = std::pair<VAddr, u64>; ///< guest address, XXH3 of the ImageInfo
using BbReplay::InsertUnique;

bool PrintOf(const VideoCore::CapturedImage& image, ImageKey& key, ImagePrint& print,
             std::string& why) {
    if (image.state.size() != sizeof(print.state)) {
        why = fmt::format("an image state blob is {} bytes, this build's is {}", image.state.size(),
                          sizeof(print.state));
        return false;
    }
    std::memcpy(&print.state, image.state.data(), sizeof(print.state));
    key = {print.state.info.guest_address, XXH3_64bits(&print.state.info, sizeof(print.state.info))};
    for (const auto& a : image.aspects) {
        print.aspects.emplace_back(fmt::format("aspect {:#x} mip {}", a.aspect, a.mip),
                                   XXH3_64bits(a.texels.data(), a.texels.size()));
    }
    return true;
}

/// The fields of two captures of one image that differ ("" when none). The uids are identities a
/// fresh cache assigns anew; they are compared through the uid mapping instead (depth links).
std::string Differences(const ImagePrint& a, const ImagePrint& b) {
    std::string out;
    const auto add = [&](const char* name) { out += out.empty() ? name : fmt::format(", {}", name); };
    const auto& s = a.state;
    const auto& t = b.state;
    if (s.flags != t.flags) add("flags");
    if (s.usage != t.usage) add("usage");
    if (s.aspect_mask != t.aspect_mask) add("aspect mask");
    if (s.num_mip_hashes != t.num_mip_hashes ||
        std::memcmp(s.mip_hashes, t.mip_hashes, sizeof(s.mip_hashes))) add("mip hashes");
    if (s.tracked != t.tracked) add("tracking");
    if (s.hash != t.hash) add("hash");
    if (a.aspects.size() != b.aspects.size()) {
        add("aspect count");
    } else {
        for (size_t i = 0; i < a.aspects.size(); ++i) {
            if (a.aspects[i] != b.aspects[i]) {
                add(a.aspects[i].first.c_str());
            }
        }
    }
    return out;
}

/// Before iteration 1: restore the captured images into the fresh cache, capture them again and
/// compare record by record (Opus-Renderer's round-trip check). Exact, or a STOP naming each
/// image and what differs.
bool RestoreImagesChecked(AmdGpu::Liverpool& lp, const std::filesystem::path& dir, u64& count,
                          std::string& why) {
    Vulkan::Rasterizer* rasterizer = AmdGpu::ReplayStateAccess::Rasterizer(lp);
    if (!rasterizer) {
        why = "no rasterizer";
        return false;
    }
    VideoCore::CapturedMetadata metadata;
    {
        Reader meta;
        MetadataRecord m{};
        if (!meta.Open(dir / "image_metadata.bin", ImageMetaMagic, why) || meta.records != 1 ||
            !meta.Get(m, why)) {
            if (why.empty()) why = "image_metadata.bin holds no record";
            return false;
        }
        metadata.version = m.version;
        metadata.state.resize(m.bytes);
        if (!meta.Read(metadata.state.data(), metadata.state.size(), why)) {
            return false;
        }
    }
    Reader images;
    if (!images.Open(dir / "images.bin", ImagesMagic, why)) {
        return false;
    }
    std::map<ImageKey, ImagePrint> captured, again;
    u64 read = 0;
    std::string read_why;
    const auto next = [&](VideoCore::CapturedImage& image) {
        if (read == images.records || !read_why.empty()) {
            return false;
        }
        ImageHeader h{};
        image = {};
        if (!images.Get(h, read_why)) {
            return false;
        }
        image.version = h.version;
        image.state.resize(h.state_bytes);
        if (!images.Read(image.state.data(), image.state.size(), read_why)) {
            return false;
        }
        for (u32 i = 0; i < h.aspects; ++i) {
            AspectRecord ar{};
            if (!images.Get(ar, read_why)) {
                return false;
            }
            VideoCore::CapturedAspect a{ar.aspect, ar.mip, ar.layers, ar.width, ar.height, ar.depth,
                                        ar.texel_bytes, std::vector<u8>(ar.bytes)};
            if (!images.Read(a.texels.data(), a.texels.size(), read_why)) {
                return false;
            }
            image.aspects.push_back(std::move(a));
        }
        ImageKey key;
        ImagePrint print;
        if (!PrintOf(image, key, print, read_why)) {
            return false;
        }
        if (!InsertUnique(captured, key, std::move(print))) {
            read_why = fmt::format("images.bin holds two records for the image at {:#x} (same "
                                   "address and ImageInfo): a round trip cannot be a bijection",
                                   key.first);
            return false;
        }
        ++read;
        return true;
    };
    VideoCore::CapturedMetadata metadata_again;
    std::string capture_why;
    u64 recaptured = 0;
    std::vector<VAddr> recaptured_twice;
    const bool restored = OnGpuThread(lp, [&] {
        if (!rasterizer->RestoreImages(next, metadata, why)) {
            return false;
        }
        return rasterizer->CaptureImages(
            [&](VideoCore::CapturedImage&& image) {
                ++recaptured;
                ImageKey key;
                ImagePrint print;
                if (PrintOf(image, key, print, capture_why) &&
                    !InsertUnique(again, key, std::move(print))) {
                    recaptured_twice.push_back(key.first);
                }
            },
            metadata_again, capture_why);
    });
    if (!read_why.empty()) {
        why = read_why;
        return false;
    }
    if (!restored) {
        why = "image restore or recapture refused: " + (why.empty() ? capture_why : why);
        return false;
    }
    if (read != images.records) {
        why = fmt::format("RestoreImages took {} of {} images", read, images.records);
        return false;
    }
    if (!capture_why.empty()) {
        why = "recapture: " + capture_why;
        return false;
    }
    if (!recaptured_twice.empty()) {
        why = fmt::format("the recapture holds two images at {:#x} (same address and ImageInfo), "
                          "{} such keys", recaptured_twice.front(), recaptured_twice.size());
        return false;
    }
    if (captured.size() != read || again.size() != recaptured || captured.size() != again.size()) {
        why = fmt::format("image counts differ: {} records read, {} distinct; {} recaptured, {} "
                          "distinct",
                          read, captured.size(), recaptured, again.size());
        return false;
    }
    std::vector<std::string> diffs;
    std::map<std::string, u64> by_field;
    std::map<u64, u64> uid_map; // captured uid -> restored uid
    for (const auto& [key, print] : captured) {
        const auto it = again.find(key);
        if (it == again.end()) {
            diffs.push_back(fmt::format("image {:#x}: missing after restore", key.first));
            ++by_field["missing"];
            continue;
        }
        if (!uid_map.emplace(print.state.image_uid, it->second.state.image_uid).second) {
            diffs.push_back(fmt::format("image {:#x}: captured uid {} recorded twice", key.first,
                                        print.state.image_uid));
            ++by_field["duplicate uid"];
        }
        if (const std::string d = Differences(print, it->second); !d.empty()) {
            diffs.push_back(fmt::format("image {:#x}: {} differ", key.first, d));
            ++by_field[d];
        }
    }
    // Depth links: the restored image's depth uid must be the restored uid of the captured link.
    for (const auto& [key, print] : captured) {
        const auto it = again.find(key);
        if (it == again.end() || (!print.state.depth_uid && !it->second.state.depth_uid)) {
            continue;
        }
        const auto mapped = uid_map.find(print.state.depth_uid);
        if (!print.state.depth_uid || mapped == uid_map.end() ||
            mapped->second != it->second.state.depth_uid) {
            diffs.push_back(fmt::format("image {:#x}: depth link {} -> {} after restore", key.first,
                                        print.state.depth_uid, it->second.state.depth_uid));
            ++by_field["depth link"];
        }
    }
    for (const auto& [key, print] : again) {
        if (!captured.count(key)) {
            diffs.push_back(fmt::format("image {:#x}: present after restore, not captured", key.first));
        }
    }
    if (metadata_again.state != metadata.state) {
        diffs.push_back("the metadata surfaces differ");
    }
    if (!diffs.empty()) {
        why = fmt::format("the image round trip is not exact ({} differences; by kind:", diffs.size());
        for (const auto& [field, n] : by_field) {
            why += fmt::format(" {} x{};", field, n);
        }
        why += ")";
        for (size_t i = 0; i < diffs.size() && i < 16; ++i) {
            why += "\n  " + diffs[i];
        }
        return false;
    }
    count = read;
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
    const auto host_resets = std::ranges::count_if(capture.resets, [](const Reset& r) { return r.host; });
    fmt::print("Perf replay: {} ({} submissions, {} reset ranges, {} of them in host memory restored "
               "in place), {} iterations; pixel output not qualified, iteration 1 is warm-up. A "
               "repeated workload, not a restored-frame replay: each iteration restores the "
               "command-processor ranges, the command words and the Liverpool state only; "
               "shader-written buffers and images and the renderer's caches carry over\n",
               dir, capture.submissions.size(), capture.resets.size(), host_resets, iterations);
    std::fflush(stdout);
    // Memory and registers first (RestoreImages needs them), then the images, checked.
    if (!QuiesceForRestore(*lp, why) ||
        !OnGpuThread(*lp, [&] { return Restore(*lp, capture, why); })) {
        std::fprintf(stderr, "STOP: perf replay: %s\n", why.c_str());
        return 3;
    }
    u64 images = 0;
    if (!RestoreImagesChecked(*lp, dir, images, why)) {
        std::fprintf(stderr, "STOP: perf replay: %s\n", why.c_str());
        return 3;
    }
    fmt::print("Perf replay: {} images restored; capture -> restore -> capture again is exact\n",
               images);
    std::fflush(stdout);
    // BB_REPLAY_GPU_WAIT=1: after each iteration also wait for its GPU work to complete
    // (Rasterizer::QuiesceForCapture) inside the timing and report submit -> GPU done. Either
    // way every iteration starts with the GPU quiesced (QuiesceForRestore, untimed).
    const char* gpu_wait_env = std::getenv("BB_REPLAY_GPU_WAIT");
    if (gpu_wait_env && std::strcmp(gpu_wait_env, "0") && std::strcmp(gpu_wait_env, "1")) {
        std::fprintf(stderr, "STOP: perf replay: BB_REPLAY_GPU_WAIT=%s (0 or 1)\n", gpu_wait_env);
        return 2;
    }
    const bool gpu_wait = gpu_wait_env && !std::strcmp(gpu_wait_env, "1");
    Vulkan::Rasterizer* rasterizer = AmdGpu::ReplayStateAccess::Rasterizer(*lp);
    const auto prep_totals = [&] {
        std::pair<u64, u64> totals{};
        OnGpuThread(*lp, [&] {
            totals = rasterizer->GetDrawPreparation().UsageTotals();
            return true;
        });
        return totals;
    };
    std::vector<double> wall_ms, cpu_ms, done_ms, busy_ms;
    BbPerf::GpuTotals gpu_last = BbPerf::Totals();
    struct MeasurementScope {
        MeasurementScope() { measurement_active.store(true, std::memory_order_release); }
        ~MeasurementScope() { measurement_active.store(false, std::memory_order_release); }
    };
    for (unsigned it = 1; it <= iterations; ++it) {
        // Untimed: the previous iteration's GPU work completes before its bytes are rewritten.
        if (!QuiesceForRestore(*lp, why)) {
            std::fprintf(stderr, "STOP: perf replay: before iteration %u: %s\n", it, why.c_str());
            return 3;
        }
        if (!OnGpuThread(*lp, [&] { return Restore(*lp, capture, why); })) {
            std::fprintf(stderr, "STOP: perf replay: %s\n", why.c_str());
            return 3;
        }
        const auto prep_before = prep_totals();
        const Counters before = Counters::Now();
        ProcessCounters process_before;
        if (!ProcessCounters::Now(process_before, why)) {
            std::fprintf(stderr, "STOP: perf replay: %s\n", why.c_str());
            return 3;
        }
        MeasurementScope measurement_scope;
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
        ProcessCounters process_after;
        if (!ProcessCounters::Now(process_after, why)) {
            std::fprintf(stderr, "STOP: perf replay: %s\n", why.c_str());
            return 3;
        }
        double gpu_done = 0;
        if (gpu_wait) {
            std::string quiet_why;
            if (!OnGpuThread(*lp, [&] { return rasterizer->QuiesceForCapture(quiet_why); })) {
                std::fprintf(stderr, "STOP: perf replay: iteration %u's GPU work did not complete: "
                                     "%s\n", it, quiet_why.c_str());
                return 3;
            }
            gpu_done = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                                 t0).count();
            done_ms.push_back(gpu_done);
        }
        // BB_PERF: GPU execution collected during this iteration's submissions = the previous
        // iteration's work when it was waited for (BB_REPLAY_GPU_WAIT=1).
        const BbPerf::GpuTotals gpu_now = BbPerf::Totals();
        const double prev_busy = gpu_now.main_ms - gpu_last.main_ms;
        const u64 prev_submissions = gpu_now.main_submissions - gpu_last.main_submissions;
        gpu_last = gpu_now;
        if (BbPerf::Enabled() && gpu_wait && it >= 3) {
            busy_ms.push_back(prev_busy); // iteration it-1 >= 2
        }
        const auto prep_after = prep_totals();
        const u64 prep_used = prep_after.first - prep_before.first;
        const u64 prep_all = prep_used + (prep_after.second - prep_before.second);
        const double gpu_cpu = (after.gpu_cpu_us - before.gpu_cpu_us) / 1000.0;
        wall_ms.push_back(ms);
        cpu_ms.push_back(gpu_cpu);
        fmt::print("Perf replay iteration {}{}: {:.2f} ms submit-to-idle{}, GPU thread CPU {:.2f} ms "
                   "(idle {:.2f} ms); prepared draws used {} of {}; {} draws, {} dispatches, {} "
                   "submissions; images registered {}, image uploads {:.1f} MiB, buffer uploads "
                   "{:.1f} MiB\n",
                   it, it == 1 ? " (warm-up)" : "", ms,
                   gpu_wait ? fmt::format(", {:.2f} ms submit-to-GPU-done", gpu_done) : std::string{},
                   gpu_cpu,
                   (after.idle_ns - before.idle_ns) / 1e6, prep_used, prep_all,
                   after.draws - before.draws,
                   after.dispatches - before.dispatches, after.submissions - before.submissions,
                   after.images - before.images,
                   (after.image_bytes - before.image_bytes) / 1048576.0,
                   (after.buffer_bytes - before.buffer_bytes) / 1048576.0);
        // Staging pool contents after the iteration (Renderer's Totals(), any thread): growth per
        // iteration with identical work would be our pools not reusing their buffers.
        {
            static constexpr const char* kStagingNames[] = {"DeviceLocal", "HostUncached",
                                                            "HostCached"};
            const auto staging = rasterizer->GetRuntime().GetStagingPool().Totals();
            std::string line;
            for (size_t i = 0; i < staging.size(); ++i) {
                const auto& t = staging[i];
                line += fmt::format("{}{} ring {} blocks {:.1f} MiB, large {} ({} held) {:.1f} MiB",
                                    i ? "; " : "", kStagingNames[i], t.ring_blocks,
                                    t.ring_bytes / 1048576.0, t.large_buffers, t.large_held,
                                    t.large_bytes / 1048576.0);
            }
            fmt::print("  staging pool after the iteration: {}\n", line);
        }
        // Read before the GPU wait: submit -> idle, the same span as the timing above.
        const auto& pb = process_before;
        const auto& pa = process_after;
        fmt::print("  process during submit-to-idle: CPU user {:.2f} ms, system {:.2f} ms; faults "
                   "{} minor, {} major; context switches {} voluntary, {} involuntary; footprint "
                   "{:.1f} MiB ({:+.1f}); host: pageins {}, pageouts {}, compressions {}, "
                   "decompressions {}, swapins {}, swapouts {}\n",
                   (pa.user_us - pb.user_us) / 1000.0, (pa.system_us - pb.system_us) / 1000.0,
                   pa.minor_faults - pb.minor_faults, pa.major_faults - pb.major_faults,
                   pa.voluntary_switches - pb.voluntary_switches,
                   pa.involuntary_switches - pb.involuntary_switches, pa.footprint / 1048576.0,
                   (double(pa.footprint) - double(pb.footprint)) / 1048576.0,
                   pa.pageins - pb.pageins, pa.pageouts - pb.pageouts,
                   pa.compressions - pb.compressions, pa.decompressions - pb.decompressions,
                   pa.swapins - pb.swapins, pa.swapouts - pb.swapouts);
        {
            // Where the CPU time ran: share on P cores, by QoS class, effective clock (cycles per
            // CPU second, from getrusage's microseconds), IPC, runnable-but-not-running time
            // relative to CPU time, energy. The task role names the app state (2 = background).
            const double ri_time = double(pa.ri_time - pb.ri_time);
            const double cpu_s = ((pa.user_us - pb.user_us) + (pa.system_us - pb.system_us)) / 1e6;
            const double cycles = double(pa.cycles - pb.cycles);
            static constexpr const char* kQos[7] = {"default", "maintenance", "background",
                                                    "utility", "legacy", "user-initiated",
                                                    "user-interactive"};
            double qos_all = 0;
            for (int q = 0; q < 7; ++q) {
                qos_all += double(pa.qos[q] - pb.qos[q]);
            }
            std::string qos_line;
            for (int q = 0; q < 7; ++q) {
                const double d = double(pa.qos[q] - pb.qos[q]);
                if (d > 0 && qos_all > 0) {
                    qos_line += fmt::format("{}{} {:.0f}%", qos_line.empty() ? "" : ", ", kQos[q],
                                            100.0 * d / qos_all);
                }
            }
            fmt::print("  cores during submit-to-idle: P-core share {:.0f}%; CPU by QoS: {}; "
                       "{:.2f} GHz effective, IPC {:.2f}; runnable not running {:.0f}% of CPU "
                       "time; energy {:.1f} mJ; task role {} (before {})\n",
                       ri_time > 0 ? 100.0 * double(pa.ri_ptime - pb.ri_ptime) / ri_time : 0.0,
                       qos_line.empty() ? std::string{"none"} : qos_line,
                       cpu_s > 0 ? cycles / cpu_s / 1e9 : 0.0,
                       cycles > 0 ? double(pa.instructions - pb.instructions) / cycles : 0.0,
                       ri_time > 0 ? 100.0 * double(pa.ri_runnable - pb.ri_runnable) / ri_time
                                   : 0.0,
                       (pa.energy_nj - pb.energy_nj) / 1e6, pa.role, pb.role);
        }
        if (BbPerf::Enabled() && gpu_wait && it >= 2) {
            fmt::print("  GPU busy of iteration {} (per-submission timestamps, {} submissions): "
                       "{:.2f} ms\n",
                       it - 1, prev_submissions, prev_busy);
        }
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
        if (!busy_ms.empty()) {
            std::vector<double> b = busy_ms;
            fmt::print("Perf replay steady state, GPU busy per iteration (iterations 2..{}): median "
                       "{:.2f} ms [{:.2f}, {:.2f}]\n",
                       iterations - 1, median(b), *std::min_element(b.begin(), b.end()),
                       *std::max_element(b.begin(), b.end()));
        }
        if (gpu_wait) {
            std::vector<double> d(done_ms.begin() + 1, done_ms.end());
            fmt::print("Perf replay steady state, GPU waited per iteration: submit-to-GPU-done median "
                       "{:.2f} ms [{:.2f}, {:.2f}]\n",
                       median(d), *std::min_element(d.begin(), d.end()),
                       *std::max_element(d.begin(), d.end()));
        }
    }
    std::fflush(stdout);
    return 0;
}

#else

extern "C" int bbgpu_replay_run(const char*, unsigned) {
    return 2;
}

#endif
