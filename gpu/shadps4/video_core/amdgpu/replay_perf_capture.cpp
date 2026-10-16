// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/amdgpu/replay_perf_capture.h"

#ifdef __APPLE__

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <type_traits>
#include <vector>
#include <fmt/format.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <sys/statvfs.h>
#include "video_core/amdgpu/cmd_snapshot.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/amdgpu/pm4_cmds.h"
#include "video_core/amdgpu/replay_format.h"
#include "video_core/amdgpu/replay_reset_walk.h"
#include "video_core/amdgpu/replay_state_access.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/texture_cache/capture_images.h"
#include "../../../../src/runtime_memory.h"


#include <unistd.h>

namespace ReplayCapture::Perf {
namespace {

using namespace ReplayFormat;
using AmdGpu::CmdSnapshot;
using AmdGpu::Liverpool;
using AmdGpu::ReplayStateAccess;

constexpr u64 MinFreeAfter = u64{40} * 1000000000; // 35 GB hard floor plus 5 GB buffer

/// One capture file: a FileHeader (record count patched at Close) and its records.
class File {
public:
    bool Open(const std::filesystem::path& path, const char (&magic)[8], std::string& why,
              u32 version = Version) {
        file = std::fopen(path.c_str(), "wb");
        if (!file) {
            why = fmt::format("cannot create {}: {}", path.string(), std::strerror(errno));
            return false;
        }
        name = path.string();
        FileHeader header{};
        std::memcpy(header.magic, magic, 8);
        header.version = file_version = version;
        std::memcpy(this->magic, magic, 8);
        return Write(&header, sizeof(header), why);
    }
    bool Write(const void* data, size_t size, std::string& why) {
        if (size && std::fwrite(data, 1, size, file) != size) {
            why = fmt::format("writing {}: {}", name, std::strerror(errno));
            return false;
        }
        bytes += size;
        return true;
    }
    template <typename T>
    bool Put(const T& value, std::string& why) {
        static_assert(std::is_trivially_copyable_v<T>);
        return Write(&value, sizeof(value), why);
    }
    void Record() {
        ++records;
    }
    bool Close(std::string& why) {
        FileHeader header{};
        std::memcpy(header.magic, magic, 8);
        header.version = file_version;
        header.records = records;
        const bool ok = std::fseek(file, 0, SEEK_SET) == 0 &&
                        std::fwrite(&header, sizeof(header), 1, file) == 1 &&
                        std::fflush(file) == 0;
        const bool closed = std::fclose(file) == 0;
        file = nullptr;
        if (!ok || !closed) {
            why = fmt::format("finishing {}: {}", name, std::strerror(errno));
            return false;
        }
        return true;
    }
    ~File() {
        if (file) {
            std::fclose(file);
        }
    }
    u64 bytes = 0, records = 0;

private:
    std::FILE* file = nullptr;
    std::string name;
    char magic[8]{};
    u32 file_version = Version;
};

/// Reads host memory without faulting: false if any byte is unreadable.
bool SafeRead(u64 address, void* out, u64 size) {
    mach_vm_size_t got = 0;
    return mach_vm_read_overwrite(mach_task_self(), address, size,
                                  reinterpret_cast<mach_vm_address_t>(out), &got) == KERN_SUCCESS &&
           got == size;
}

/// Guest memory as the capture sees it: the views and blocks of one VMA-table read, the
/// backing pool and the eboot image range.
struct Memory {
    std::vector<RuntimeMemoryView> views;
    std::vector<RuntimeMemoryBlock> blocks;
    u64 generation = 0;
    const u8* backing = nullptr;
    u64 direct_bytes = 0, total_bytes = 0;
    uintptr_t image_start = 0;
    u64 image_size = 0;

    bool Load(std::string& why) {
        size_t nv = 0, nb = 0;
        for (int attempt = 0; attempt < 8; ++attempt) {
            views.resize(nv + 64);
            blocks.resize(nb + 64);
            const size_t cv = views.size(), cb = blocks.size();
            if (runtime_memory_layout(views.data(), cv, &nv, blocks.data(), cb, &nb,
                                      &generation) != 0) {
                why = "runtime_memory_layout failed";
                return false;
            }
            if (nv <= cv && nb <= cb) {
                views.resize(nv);
                blocks.resize(nb);
                backing = static_cast<const u8*>(runtime_memory_backing(&direct_bytes, &total_bytes));
                if (!backing) {
                    why = "no backing pool";
                    return false;
                }
                if (runtime_guest_image(&image_start, &image_size) != 0 || !image_start ||
                    !image_size) {
                    why = "no guest image range (runtime_guest_image)";
                    return false;
                }
                return true;
            }
        }
        why = "the VMA table kept growing while it was read";
        return false;
    }

    /// Bytes at a guest VA at the cut, through the backing pool (no guest-view access), the
    /// image, or, outside both, host memory (`host` set: e.g. an HLE object in the low arena that
    /// a packet writes); false when unreadable.
    bool Read(u64 va, void* out, u64 size, bool* host = nullptr) const {
        if (host) {
            *host = false;
        }
        if (va >= image_start && size <= image_size && va - image_start <= image_size - size) {
            return SafeRead(va, out, size); // protected regions inside the image fail, not fault
        }
        const auto it = std::upper_bound(views.begin(), views.end(), va,
                                         [](u64 a, const RuntimeMemoryView& v) { return a < v.start; });
        const RuntimeMemoryView* v = it == views.begin() ? nullptr : &*(it - 1);
        if (!v || va >= v->end) {
            if (host) {
                *host = true;
            }
            return SafeRead(va, out, size);
        }
        if (size > v->end - va || v->phys + (va - v->start) + size > total_bytes) {
            return false; // straddles a view's end
        }
        std::memcpy(out, backing + v->phys + (va - v->start), size);
        return true;
    }
};

bool WriteViews(const std::filesystem::path& dir, const Memory& m, std::string& why) {
    File f;
    if (!f.Open(dir / "views.bin", ViewsMagic, why) ||
        !f.Put(ViewsCounts{m.blocks.size(), m.views.size()}, why)) {
        return false;
    }
    for (const auto& b : m.blocks) {
        if (!f.Put(BlockRecord{b.start, b.size, b.type, 0}, why)) {
            return false;
        }
        f.Record();
    }
    for (const auto& v : m.views) {
        if (!f.Put(ViewRecord{v.start, v.end, v.phys, u32(v.flexible), v.prot, v.type, 0}, why)) {
            return false;
        }
        f.Record();
    }
    return f.Close(why);
}

/// The backing pages that ever held data: direct blocks and flexible views, page by page per
/// the VM's disposition (a never-touched page reads as zero and is not written: reading it
/// would commit it).
bool WriteBacking(const std::filesystem::path& dir, const Memory& m, u64& payload, std::string& why) {
    std::vector<std::pair<u64, u64>> ranges; // [phys, end)
    for (const auto& b : m.blocks) {
        ranges.emplace_back(b.start, b.start + b.size);
    }
    for (const auto& v : m.views) {
        if (v.flexible) {
            ranges.emplace_back(v.phys, v.phys + (v.end - v.start));
        }
    }
    std::sort(ranges.begin(), ranges.end());
    std::vector<std::pair<u64, u64>> merged;
    for (const auto& r : ranges) {
        if (!merged.empty() && r.first <= merged.back().second) {
            merged.back().second = std::max(merged.back().second, r.second);
        } else {
            merged.push_back(r);
        }
    }
    File f;
    if (!f.Open(dir / "backing.bin", BackingMagic, why)) {
        return false;
    }
    const u64 page = vm_page_size;
    constexpr u64 Chunk = u64{64} << 20;
    std::vector<int> disposition(Chunk / page);
    payload = 0;
    for (const auto& [lo, hi] : merged) {
        if (hi > m.total_bytes || lo % page || hi % page) {
            why = fmt::format("backing range [{:#x}, {:#x}) outside the pool or unaligned", lo, hi);
            return false;
        }
        for (u64 at = lo; at < hi; at += Chunk) {
            const u64 size = std::min(Chunk, hi - at);
            mach_vm_size_t count = size / page;
            const kern_return_t kr = mach_vm_page_range_query(
                mach_task_self(), reinterpret_cast<mach_vm_address_t>(m.backing + at), size,
                reinterpret_cast<mach_vm_address_t>(disposition.data()), &count);
            if (kr != KERN_SUCCESS || count != size / page) {
                why = fmt::format("page query of backing [{:#x}, +{:#x}) failed ({})", at, size, kr);
                return false;
            }
            for (u64 p = 0; p < count;) {
                if (!disposition[p]) {
                    ++p;
                    continue;
                }
                u64 q = p;
                while (q < count && disposition[q]) {
                    ++q;
                }
                const u64 phys = at + p * page, bytes = (q - p) * page;
                if (!f.Put(BackingRun{phys, bytes}, why) || !f.Write(m.backing + phys, bytes, why)) {
                    return false;
                }
                f.Record();
                payload += bytes;
                p = q;
            }
        }
    }
    return f.Close(why);
}

/// The eboot image as runs of readable pages: it holds protected regions (a plain read of them
/// faults; Playtest's first image write failed with EFAULT).
bool WriteImage(const std::filesystem::path& dir, const Memory& m, u64& readable, std::string& why) {
    File f;
    if (!f.Open(dir / "image.bin", ImageMagic, why) ||
        !f.Put(ImageRecord{m.image_start, m.image_size}, why)) {
        return false;
    }
    const u64 page = vm_page_size;
    std::vector<u8> buffer(page);
    readable = 0;
    u64 run_start = 0, run_bytes = 0;
    std::vector<u8> run;
    const auto flush = [&]() {
        if (!run_bytes) {
            return true;
        }
        if (!f.Put(ImageRun{run_start, run_bytes}, why) || !f.Write(run.data(), run.size(), why)) {
            return false;
        }
        f.Record();
        readable += run_bytes;
        run.clear();
        run_bytes = 0;
        return true;
    };
    for (u64 off = 0; off < m.image_size; off += page) {
        const u64 n = std::min(page, m.image_size - off);
        if (SafeRead(m.image_start + off, buffer.data(), n)) {
            if (!run_bytes) {
                run_start = off;
            }
            run.insert(run.end(), buffer.begin(), buffer.begin() + n);
            run_bytes += n;
        } else if (!flush()) {
            return false;
        }
    }
    return flush() && f.Close(why);
}

bool WriteLiverpool(const std::filesystem::path& dir, const Liverpool& lp, std::string& why) {
    static_assert(std::is_trivially_copyable_v<AmdGpu::Regs>);
    static_assert(std::is_trivially_copyable_v<AmdGpu::ComputeProgram>);
    File f;
    if (!f.Open(dir / "liverpool.bin", LiverpoolMagic, why)) {
        return false;
    }
    const auto section = [&](LiverpoolSection s, const void* data, u64 size) {
        if (!f.Put(SectionRecord{s, 0, size}, why) || !f.Write(data, size, why)) {
            return false;
        }
        f.Record();
        return true;
    };
    const auto& ce = ReplayStateAccess::Ce(lp);
    const u32 counters[3] = {ce.ce_compare_count, ce.ce_count, ce.de_count};
    const VAddr indirect = ReplayStateAccess::IndirectArgs(lp);
    if (!section(LiverpoolSection::Regs, &lp.regs, sizeof(lp.regs)) ||
        !section(LiverpoolSection::CeCounters, counters, sizeof(counters)) ||
        !section(LiverpoolSection::CeHeap, ReplayStateAccess::CeHeap().data(),
                 ReplayStateAccess::CeHeap().size()) ||
        !section(LiverpoolSection::IndirectArgs, &indirect, sizeof(indirect)) ||
        !section(LiverpoolSection::GfxRegChecksum, &lp.gfx_reg_checksum,
                 sizeof(lp.gfx_reg_checksum))) {
        return false;
    }
    for (u32 q = 0; q < Liverpool::NumTotalQueues; ++q) {
        struct {
            u32 queue;
            AmdGpu::ComputeProgram cs;
        } entry{q, ReplayStateAccess::CsState(lp, q)};
        if (!section(LiverpoolSection::QueueCsState, &entry, sizeof(entry))) {
            return false;
        }
    }
    return f.Close(why);
}

bool WriteSubmissions(const std::filesystem::path& dir,
                      std::span<const std::shared_ptr<CmdSnapshot>> held, u64& words,
                      std::string& why) {
    File f;
    if (!f.Open(dir / "submissions.bin", SubmissionsMagic, why)) {
        return false;
    }
    words = 0;
    for (const auto& snapshot : held) {
        const auto regions = snapshot->Regions();
        const CmdSnapshot::Region* dcb = &snapshot->Dcb();
        const CmdSnapshot::Region* ccb = snapshot->Ccb();
        SubmissionRecord s{snapshot->submit_number, u32(regions.size()),
                           s32(dcb - regions.data()), ccb ? s32(ccb - regions.data()) : -1, 0};
        if (!f.Put(s, why)) {
            return false;
        }
        for (const auto& r : regions) {
            if (!f.Put(RegionRecord{reinterpret_cast<u64>(r.guest), r.size_dw, u32(r.constant),
                                    u32(r.copied), 0},
                       why)) {
                return false;
            }
            if (r.copied) {
                const auto w = snapshot->Words(r);
                if (!f.Write(w.data(), w.size_bytes(), why)) {
                    return false;
                }
                words += w.size();
            }
        }
        f.Record();
    }
    return f.Close(why);
}

/// The frame's command-processor memory traffic, in execution order (CCB, then DCB with every
/// indirect buffer where it is called): what each iteration must find as it was at the cut.
using Detail::ResetWalk;
using Detail::WalkRegion;

/// One held submission as the reset walk sees it: every captured region with its words.
void WalkSubmission(ResetWalk& walk, const CmdSnapshot& snapshot_) {
    auto& snapshot = const_cast<CmdSnapshot&>(snapshot_);
    const auto view = [&](const CmdSnapshot::Region& r) {
        return WalkRegion{reinterpret_cast<u64>(r.guest), r.size_dw, r.constant, r.copied,
                          r.copied ? snapshot.Words(r) : std::span<const u32>{}};
    };
    std::vector<WalkRegion> regions;
    for (const auto& r : snapshot.Regions()) {
        regions.push_back(view(r));
    }
    const CmdSnapshot::Region* ccb = snapshot.Ccb();
    const WalkRegion ccb_view = ccb ? view(*ccb) : WalkRegion{};
    walk.Submission(regions, ccb ? &ccb_view : nullptr, view(snapshot.Dcb()));
}

/// Every GPU-modified image (Rasterizer::CaptureImages, GPU command thread after
/// QuiesceForCapture), streamed: one image's texels in memory at a time.
bool WriteImages(const std::filesystem::path& dir, const Liverpool& lp, u64& count,
                 u64& texel_bytes, std::string& why) {
    Vulkan::Rasterizer* rasterizer = ReplayStateAccess::Rasterizer(lp);
    if (!rasterizer) {
        why = "no rasterizer";
        return false;
    }
    File images;
    if (!images.Open(dir / "images.bin", ImagesMagic, why)) {
        return false;
    }
    bool write_ok = true;
    std::string write_why;
    VideoCore::CapturedMetadata metadata;
    count = texel_bytes = 0;
    const bool captured = rasterizer->CaptureImages(
        [&](VideoCore::CapturedImage&& image) {
            if (!write_ok) {
                return;
            }
            write_ok = images.Put(ImageHeader{image.version, u32(image.aspects.size()),
                                              image.state.size()},
                                  write_why) &&
                       images.Write(image.state.data(), image.state.size(), write_why);
            for (const auto& a : image.aspects) {
                if (!write_ok) {
                    break;
                }
                write_ok = images.Put(AspectRecord{a.aspect, a.mip, a.layers, a.width, a.height,
                                                   a.depth, a.texel_bytes, 0, a.texels.size()},
                                      write_why) &&
                           images.Write(a.texels.data(), a.texels.size(), write_why);
                texel_bytes += a.texels.size();
            }
            images.Record();
            ++count;
        },
        metadata, why);
    if (!captured) {
        why = "image capture refused: " + why;
        return false;
    }
    if (!write_ok) {
        why = write_why;
        return false;
    }
    File meta;
    if (!images.Close(why) || !meta.Open(dir / "image_metadata.bin", ImageMetaMagic, why) ||
        !meta.Put(MetadataRecord{metadata.version, 0, metadata.state.size()}, why) ||
        !meta.Write(metadata.state.data(), metadata.state.size(), why)) {
        return false;
    }
    meta.Record();
    return meta.Close(why);
}

bool WriteResets(const std::filesystem::path& dir, const Memory& m, ResetWalk& walk,
                 u64& count, u64& bytes, u64& host_count, std::string& why) {
    auto ranges = walk.ranges;
    std::ranges::sort(ranges, [](const auto& a, const auto& b) { return a.address < b.address; });
    std::vector<ResetWalk::Range> merged;
    for (const auto& r : ranges) {
        if (!merged.empty() && r.address <= merged.back().address + merged.back().bytes) {
            auto& last = merged.back();
            last.bytes = std::max(last.address + last.bytes, r.address + r.bytes) - last.address;
        } else {
            merged.push_back(r);
        }
    }
    if (merged.size() > MaxResetRecords) {
        why = "more than 1000000 reset ranges";
        return false;
    }
    File f;
    if (!f.Open(dir / "resets.bin", ResetsMagic, why, ResetsVersion)) {
        return false;
    }
    std::vector<u8> value;
    count = bytes = host_count = 0;
    for (const auto& r : merged) {
        if (!r.bytes || r.bytes > MaxResetBytes) {
            why = fmt::format("reset range {:#x} is {} bytes", r.address, r.bytes);
            return false;
        }
        value.resize(r.bytes);
        bool host = false;
        if (!m.Read(r.address, value.data(), r.bytes, &host)) {
            why = fmt::format("reset range [{:#x}, +{:#x}) (packet {:#04x}) is unreadable or "
                              "straddles a guest view",
                              r.address, r.bytes, r.opcode);
            return false;
        }
        host_count += host;
        HostObjectRecord object{};
        if (host) {
            RuntimeReplayObject o{};
            if (runtime_replay_object(r.address, &o) != 0 || r.bytes > o.size ||
                r.address < o.start || r.address - o.start > o.size - r.bytes) {
                why = fmt::format("reset range [{:#x}, +{:#x}) (packet {:#04x}) is host memory "
                                  "outside an explicitly named live range",
                                  r.address, r.bytes, r.opcode);
                return false;
            }
            object.start = o.start;
            object.size = o.size;
            object.offset = r.address - o.start;
            std::memcpy(object.name, o.name, sizeof(object.name));
        }
        if (!f.Put(ResetRecord{r.address, u32(r.bytes), r.kind, r.opcode, u32(host)}, why) ||
            !f.Write(value.data(), value.size(), why) || (host && !f.Put(object, why))) {
            return false;
        }
        f.Record();
        ++count;
        bytes += r.bytes;
    }
    return f.Close(why);
}

} // namespace

std::string Write(const Liverpool& liverpool, std::span<const std::shared_ptr<CmdSnapshot>> held,
                  std::string& why) {
    Memory memory;
    if (!memory.Load(why)) {
        return {};
    }
    // The reset walk first: a frame the replay cannot run is refused before anything is written.
    ResetWalk walk{[&](u64 address, void* out, u64 bytes) { return memory.Read(address, out, bytes); },
                   ReplayStateAccess::CounterPairs(liverpool)};
    for (const auto& snapshot : held) {
        WalkSubmission(walk, *snapshot);
    }
    if (!walk.blockers.empty()) {
        why = "the frame's command-processor memory traffic cannot be replayed:";
        for (const auto& b : walk.blockers) {
            why += "\n      - " + b;
        }
        return {};
    }

    const char* base_env = std::getenv("BB_REPLAY_DIR");
    const std::filesystem::path base = base_env && *base_env ? base_env : "out/replay";
    std::error_code ec;
    std::filesystem::create_directories(base, ec);
    if (ec) {
        why = fmt::format("cannot create {}: {}", base.string(), ec.message());
        return {};
    }
    // Another capture's *.incomplete directory (one still being written, or left by a crash) is
    // never touched here: named, so whoever owns it can remove it.
    for (const auto& entry : std::filesystem::directory_iterator(base, ec)) {
        if (entry.is_directory() && entry.path().extension() == ".incomplete") {
            fmt::print("  perf capture: {} is an unfinished capture (another one running, or a "
                       "crash's leftover); not touched\n",
                       entry.path().string());
        }
    }
    ec.clear();
    u64 estimate = memory.image_size + (u64{2} << 30); // images: ~0.5 GiB in the clinic
    for (const auto& b : memory.blocks) {
        estimate += b.size;
    }
    for (const auto& v : memory.views) {
        estimate += v.flexible ? v.end - v.start : 0;
    }
    struct statvfs fs{};
    if (statvfs(base.c_str(), &fs) != 0) {
        why = fmt::format("statvfs {}: {}", base.string(), std::strerror(errno));
        return {};
    }
    const u64 free_bytes = u64(fs.f_bavail) * fs.f_frsize;
    if (free_bytes < estimate || free_bytes - estimate < MinFreeAfter) {
        why = fmt::format("{:.1f} GiB free, the capture needs up to {:.1f} GiB and must leave {} GiB: "
                          "capture refused; preserve unique evidence and coordinate disk space under {}",
                          free_bytes / 1073741824.0, estimate / 1073741824.0, MinFreeAfter >> 30,
                          base.string());
        return {};
    }

    // This capture's own directory: time and process id, created exclusively (an existing one
    // is someone else's), renamed to its final name only when complete.
    char stamp[32];
    const std::time_t now = std::time(nullptr);
    std::strftime(stamp, sizeof(stamp), "frame-%Y%m%d-%H%M%S", std::localtime(&now));
    const std::string name = fmt::format("{}-{}", stamp, getpid());
    const std::filesystem::path final_dir = base / name;
    const std::filesystem::path dir = base / (name + ".incomplete");
    if (std::filesystem::exists(final_dir, ec) || ec) {
        why = fmt::format("{} exists already", final_dir.string());
        return {};
    }
    if (!std::filesystem::create_directory(dir, ec) || ec) {
        why = fmt::format("cannot create {} exclusively: {}", dir.string(),
                          ec ? ec.message() : "it exists");
        return {};
    }
    const auto fail = [&]() -> std::string {
        std::error_code ignored;
        std::filesystem::remove_all(dir, ignored); // never leave a partial capture
        return {};
    };

    u64 backing_bytes = 0, words = 0, resets = 0, reset_bytes = 0, images = 0, texel_bytes = 0,
        image_readable = 0, host_resets = 0;
    if (!WriteViews(dir, memory, why) || !WriteBacking(dir, memory, backing_bytes, why) ||
        !WriteImage(dir, memory, image_readable, why) || !WriteLiverpool(dir, liverpool, why) ||
        !WriteSubmissions(dir, held, words, why) ||
        !WriteResets(dir, memory, walk, resets, reset_bytes, host_resets, why) ||
        !WriteImages(dir, liverpool, images, texel_bytes, why)) {
        return fail();
    }
    // Written last: a capture without a manifest is not a capture. It records every file's size,
    // which the loader checks before it maps or writes anything (src/probe.c).
    std::string files;
    for (const char* file : CaptureFiles) {
        const u64 size = std::filesystem::file_size(dir / file, ec);
        if (ec) {
            why = fmt::format("{}: {}", file, ec.message());
            return fail();
        }
        files += fmt::format("{}\n    \"{}\": {}", files.empty() ? "" : ",", file, size);
    }
    std::FILE* manifest = std::fopen((dir / "manifest.json").c_str(), "w");
    if (!manifest) {
        why = fmt::format("cannot create manifest.json: {}", std::strerror(errno));
        return fail();
    }
    fmt::print(manifest,
               "{{\n  \"format\": \"bbport perf replay\",\n  \"version\": {},\n"
               "  \"manifest_version\": 2,\n  \"files\": {{{}\n  }},\n"
               "  \"pixel_output\": \"not qualified (perf replay)\",\n"
               "  \"vma_generation\": {},\n  \"blocks\": {},\n  \"views\": {},\n"
               "  \"backing_payload_bytes\": {},\n  \"image\": [{}, {}],\n"
               "  \"image_readable_bytes\": {},\n"
               "  \"submissions\": {},\n  \"command_dwords\": {},\n"
               "  \"resets\": {},\n  \"reset_bytes\": {},\n  \"host_resets\": {},\n"
               "  \"images\": {},\n  \"image_texel_bytes\": {}\n}}\n",
               Version, files, memory.generation, memory.blocks.size(), memory.views.size(), backing_bytes,
               memory.image_start, memory.image_size, image_readable, held.size(), words, resets,
               reset_bytes, host_resets,
               images, texel_bytes);
    if (std::fclose(manifest) != 0) {
        why = "writing manifest.json failed";
        return fail();
    }
    if (std::filesystem::exists(final_dir, ec) || ec) {
        why = fmt::format("{} appeared while the capture was written", final_dir.string());
        return fail();
    }
    std::filesystem::rename(dir, final_dir, ec);
    if (ec) {
        why = fmt::format("cannot rename {} to {}: {}", dir.string(), final_dir.string(),
                          ec.message());
        return fail();
    }
    return final_dir.string();
}

} // namespace ReplayCapture::Perf

#else

namespace ReplayCapture::Perf {
std::string Write(const AmdGpu::Liverpool&, std::span<const std::shared_ptr<AmdGpu::CmdSnapshot>>,
                  std::string& why) {
    why = "the perf replay capture is Apple-only";
    return {};
}
} // namespace ReplayCapture::Perf

#endif
