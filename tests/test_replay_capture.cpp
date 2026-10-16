// SPDX-License-Identifier: GPL-2.0-or-later
// Replay capture inventory (replay_capture.h): the environment parser accepts decimal digits
// within the bound only, and the PM4 census counts whole packets only, stopping a region (and
// marking the census incomplete) at an invalid header or a packet past the region's end. The perf
// capture's reset walk refuses truncated packets, unknown opcodes, wrapping ranges and waits that
// only a conditional or constant-engine write would satisfy; the image round trip refuses
// duplicate keys.
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <map>
#include "bbport_replay_compare.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/amdgpu/replay_capture.h"
#include "video_core/amdgpu/replay_format.h"
#include "video_core/amdgpu/replay_reset_walk.h"
#include "video_core/amdgpu/replay_state_access.h"

extern "C" int bbgpu_replay_check_file(const char*, const char*, char*, size_t);

using ReplayCapture::Detail::CountRegion;
using ReplayCapture::Detail::ParseBounded;
using ReplayCapture::Detail::Pm4Census;

namespace {

unsigned failures = 0;

void Check(bool ok, const char* what) {
    std::fprintf(stderr, "%-58s %s\n", what, ok ? "ok" : "FAIL");
    failures += ok ? 0 : 1;
}

void Parse(std::string_view text, u64 max, std::optional<u64> want) {
    const auto got = ParseBounded(text, max);
    const bool ok = got == want;
    std::fprintf(stderr, "parse \"%.*s\" max %llu -> %s %s\n", int(text.size()), text.data(),
                 static_cast<unsigned long long>(max),
                 got ? std::to_string(*got).c_str() : "rejected", ok ? "ok" : "FAIL");
    failures += ok ? 0 : 1;
}

constexpr u32 Type3(u32 op, u32 payload_dwords) {
    return (3u << 30) | ((payload_dwords - 1) << 16) | (op << 8);
}
constexpr u32 Type0(u32 reg, u32 values) {
    return ((values - 1) << 16) | reg;
}
constexpr u32 Type2 = 2u << 30;
constexpr u32 DrawIndexAuto = 0x2D, Rewind = 0x59, Nop = 0x10;

using ReplayCapture::Perf::Detail::ResetWalk;
using ReplayCapture::Perf::Detail::WalkRegion;

constexpr u32 Eop = 0x47, WaitRegMem = 0x3C, CondExec = 0x22, WriteData = 0x37, DumpConstRam = 0x83;

/// EVENT_WRITE_EOP writing a 32-bit value to `address`.
std::vector<u32> EopTo(u64 address) {
    return {Type3(Eop, 5), 0x504, u32(address), u32(address >> 32) | (1u << 29), 1, 0};
}
/// WAIT_REG_MEM on memory: `address` == ref.
std::vector<u32> WaitFor(u64 address, u32 ref) {
    return {Type3(WaitRegMem, 6), 3u | (1u << 4), u32(address), u32(address >> 32), ref, ~0u, 10};
}

/// Runs the walk over `dcb` (and `ccb`); memory reads zero everywhere.
ResetWalk Walk(const std::vector<u32>& dcb, const std::vector<u32>& ccb = {}) {
    ResetWalk walk{[](u64, void* out, u64 bytes) {
                       std::memset(out, 0, bytes);
                       return true;
                   },
                   1};
    std::vector<WalkRegion> regions;
    const WalkRegion d{0x7000000000, u32(dcb.size()), false, true, dcb};
    const WalkRegion c{0x7000100000, u32(ccb.size()), true, true, ccb};
    regions.push_back(d);
    if (!ccb.empty()) {
        regions.push_back(c);
    }
    walk.Submission(regions, ccb.empty() ? nullptr : &c, d);
    return walk;
}

bool Blocked(const ResetWalk& walk, std::string_view text) {
    for (const auto& b : walk.blockers) {
        if (b.find(text) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::vector<u32> Cat(std::initializer_list<std::vector<u32>> parts) {
    std::vector<u32> out;
    for (const auto& part : parts) {
        out.insert(out.end(), part.begin(), part.end());
    }
    return out;
}

Pm4Census Scan(const std::vector<u32>& words) {
    Pm4Census census;
    CountRegion(census, words);
    return census;
}

template <typename T>
void Append(std::vector<u8>& bytes, const T& value) {
    const auto* at = reinterpret_cast<const u8*>(&value);
    bytes.insert(bytes.end(), at, at + sizeof(value));
}

void ReaderControls() {
    char temporary[] = "/tmp/bb-replay-reader-XXXXXX";
    char* created = mkdtemp(temporary);
    Check(created != nullptr, "production reader: temporary directory created");
    if (!created) return;
    const std::filesystem::path dir{created};
    const auto write = [&](const char* name, const std::vector<u8>& bytes) {
        std::ofstream out{dir / name, std::ios::binary | std::ios::trunc};
        out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        return bool(out);
    };
    const auto accepts = [&](const char* name, const std::vector<u8>& bytes) {
        char why[256]{};
        if (!write(name, bytes)) return false;
        return bbgpu_replay_check_file(dir.c_str(), name, why, sizeof(why)) == 0;
    };
    using namespace ReplayFormat;
    const FileHeader reset_header{{'B','B','R','P','R','S','E','T'}, ResetsVersion, 0, 2};
    const ResetRecord guest{0x1000, 4, ResetKind::Written, 0x37, 0};
    const ResetRecord host{0x1020, 4, ResetKind::Written, 0x37, 1};
    const HostObjectRecord object{0x1000, 0x80, 0x20, "named host"};
    const u32 value = 0x12345678;
    std::vector<u8> resets;
    Append(resets, reset_header); Append(resets, guest); Append(resets, value);
    Append(resets, host); Append(resets, value); Append(resets, object);
    Check(accepts("resets.bin", resets), "production reader: complete named v3 resets accepted");
    auto altered = resets;
    const u32 old_version = 2;
    std::memcpy(altered.data() + 8, &old_version, 4);
    Check(!accepts("resets.bin", altered), "production reader: legacy v2 resets rejected");
    altered = resets; altered.pop_back();
    Check(!accepts("resets.bin", altered), "production reader: truncated last identity rejected");
    altered = resets; altered.push_back(0);
    Check(!accepts("resets.bin", altered), "production reader: trailing reset byte rejected");
    altered = resets;
    const u64 wrong_offset = 0x21;
    std::memcpy(altered.data() + altered.size() - sizeof(HostObjectRecord) + 16,
                &wrong_offset, sizeof(wrong_offset));
    Check(!accepts("resets.bin", altered), "production reader: wrong last host offset rejected");
    altered = resets;
    const u32 too_large = MaxResetBytes + 1;
    std::memcpy(altered.data() + sizeof(FileHeader) + 8, &too_large, sizeof(too_large));
    Check(!accepts("resets.bin", altered), "production reader: oversized reset refused before allocation");
    altered = resets;
    altered.resize(sizeof(FileHeader) + sizeof(ResetRecord));
    Check(!accepts("resets.bin", altered), "production reader: reset payload refused before allocation");
    altered = resets;
    altered.resize(resets.size() - sizeof(HostObjectRecord));
    Check(!accepts("resets.bin", altered), "production reader: missing host identity refused before allocation");
    altered = resets;
    const u64 overlapping = 0x1002;
    const size_t host_record = sizeof(FileHeader) + sizeof(ResetRecord) + sizeof(value);
    std::memcpy(altered.data() + host_record, &overlapping, sizeof(overlapping));
    Check(!accepts("resets.bin", altered), "production reader: overlapping reset ranges rejected");

    const auto liverpool = [&](bool counters, bool indirect, bool duplicate, bool trailing) {
        std::vector<u8> bytes;
        const u64 sections = 3 + u64(counters) + u64(indirect) +
                             AmdGpu::Liverpool::NumTotalQueues + u64(duplicate);
        const FileHeader header{{'B','B','R','P','L','P','O','O'}, Version, 0, sections};
        Append(bytes, header);
        const auto section = [&](LiverpoolSection kind, const void* data, size_t size) {
            Append(bytes, SectionRecord{kind, 0, size});
            const auto* at = reinterpret_cast<const u8*>(data);
            bytes.insert(bytes.end(), at, at + size);
        };
        AmdGpu::Regs regs{};
        section(LiverpoolSection::Regs, &regs, sizeof(regs));
        const u32 ce[3]{};
        if (counters) section(LiverpoolSection::CeCounters, ce, sizeof(ce));
        const auto heap = AmdGpu::ReplayStateAccess::CeHeap();
        section(LiverpoolSection::CeHeap, heap.data(), heap.size());
        const u64 zero = 0;
        if (indirect) section(LiverpoolSection::IndirectArgs, &zero, sizeof(zero));
        section(LiverpoolSection::GfxRegChecksum, &zero, sizeof(zero));
        if (duplicate) section(LiverpoolSection::GfxRegChecksum, &zero, sizeof(zero));
        for (u32 q = 0; q < AmdGpu::Liverpool::NumTotalQueues; ++q) {
            struct { u32 queue; AmdGpu::ComputeProgram cs; } entry{q, {}};
            section(LiverpoolSection::QueueCsState, &entry, sizeof(entry));
        }
        if (trailing) bytes.push_back(0);
        return bytes;
    };
    Check(accepts("liverpool.bin", liverpool(true, true, false, false)),
          "production reader: complete Liverpool state accepted");
    Check(!accepts("liverpool.bin", liverpool(false, true, false, false)),
          "production reader: missing CE counters rejected");
    Check(!accepts("liverpool.bin", liverpool(true, false, false, false)),
          "production reader: missing indirect args rejected");
    Check(!accepts("liverpool.bin", liverpool(true, true, true, false)),
          "production reader: duplicate singleton rejected");
    Check(!accepts("liverpool.bin", liverpool(true, true, false, true)),
          "production reader: trailing Liverpool byte rejected");
    const auto submissions = [&](s32 ccb_index, u32 size_dw) {
        std::vector<u8> bytes;
        Append(bytes, FileHeader{{'B','B','R','P','S','U','B','S'}, Version, 0, 1});
        Append(bytes, SubmissionRecord{1, 1, 0, ccb_index, 0});
        Append(bytes, RegionRecord{0x1000, size_dw, 0, 1, 0});
        if (size_dw == 1) Append(bytes, value);
        return bytes;
    };
    Check(!accepts("submissions.bin", submissions(-2, 1)),
          "production reader: invalid negative CCB index rejected");
    Check(!accepts("submissions.bin", submissions(0, 1)),
          "production reader: DCB and CCB cannot share an index");
    Check(!accepts("submissions.bin", submissions(-1, UINT32_MAX)),
          "production reader: wrapped command byte count rejected");
    std::filesystem::remove_all(dir);
}

} // namespace

int main() {
    ReaderControls();
    constexpr u64 max64 = std::numeric_limits<u64>::max();
    Parse("1", 100000, 1);
    Parse("00012", 100000, 12);
    Parse("0", 100000, 0); // in range; the capture rejects zero itself
    Parse("100000", 100000, 100000);
    Parse("100001", 100000, std::nullopt);
    Parse("", 100000, std::nullopt);
    Parse("+1", 100000, std::nullopt);
    Parse("-1", 100000, std::nullopt);
    Parse(" 1", 100000, std::nullopt);
    Parse("1 ", 100000, std::nullopt);
    Parse("1x", 100000, std::nullopt);
    Parse("0x10", 100000, std::nullopt);
    Parse("1e3", 100000, std::nullopt);
    Parse("18446744073709551615", max64, max64);
    Parse("18446744073709551616", max64, std::nullopt); // wraps to 0 without the check
    Parse("99999999999999999999999", max64, std::nullopt);
    Parse("60001", 60000, std::nullopt);
    Parse("0", 0, 0);
    Parse("9", 0, std::nullopt); // max - digit would wrap
    Parse("5", 5, 5);
    Parse("6", 5, std::nullopt);
    Parse("05", 5, 5);
    Parse("10", 5, std::nullopt);

    {
        const auto c = Scan({Type3(DrawIndexAuto, 2), 3, 0, Type2, Type0(0x2000, 1), 7,
                             Type3(Nop, 1), 0});
        Check(c.packets == 2 && c.opcodes[DrawIndexAuto] == 1 && c.opcodes[Nop] == 1 &&
                  c.type2 == 1 && c.type0 == 1 && c.Complete(),
              "well-formed stream: whole packets, complete");
    }
    {
        const auto c = Scan({});
        Check(c.packets == 0 && c.Complete(), "empty region: complete");
    }
    {
        const auto c = Scan({Type3(DrawIndexAuto, 2)});
        Check(c.packets == 0 && c.truncated_regions == 1 && !c.Complete(),
              "header alone, payload past the end: truncated, not counted");
    }
    {
        const auto c = Scan({Type3(Nop, 1), 0, Type3(DrawIndexAuto, 4), 1, 2});
        Check(c.packets == 1 && c.opcodes[DrawIndexAuto] == 0 && c.truncated_regions == 1 &&
                  !c.Complete(),
              "payload truncated after a whole packet: one counted");
    }
    {
        const auto c = Scan({Type0(0x2000, 3), 1, 2});
        Check(c.type0 == 0 && c.truncated_regions == 1, "type-0 values past the end: truncated");
    }
    {
        const auto c = Scan({Type3(Nop, 1), 0, 1u << 30, Type3(DrawIndexAuto, 2), 0, 0});
        Check(c.packets == 1 && c.opcodes[DrawIndexAuto] == 0 && c.bad_header_regions == 1 &&
                  !c.Complete(),
              "type-1 header: scan stops, later packets not counted");
    }
    {
        std::vector<u32> words(0x3FFF + 2, 0);
        words[0] = (3u << 30) | (0x3FFFu << 16) | (Nop << 8);
        const auto fits = Scan(words);
        words.pop_back();
        const auto short_by_one = Scan(words);
        Check(fits.packets == 1 && fits.Complete() && short_by_one.truncated_regions == 1,
              "largest count: exact fit counted, one dword short truncated");
    }
    {
        const auto c = Scan({Type3(Rewind, 1), 0, Type3(DrawIndexAuto, 2), 0, 0});
        Check(c.opcodes[Rewind] == 1 && c.packets == 2 && c.Complete(), "REWIND counted");
    }
    {
        Pm4Census c;
        c.uncopied_regions = 1;
        Check(!c.Complete(), "an uncopied region leaves the census incomplete");
    }
    {
        Pm4Census c;
        CountRegion(c, std::vector<u32>{Type3(Nop, 1), 0});
        CountRegion(c, std::vector<u32>{1u << 30});
        CountRegion(c, std::vector<u32>{Type3(Nop, 1), 0});
        Check(c.packets == 2 && c.bad_header_regions == 1,
              "a bad region stops only itself; later regions scanned");
    }
    {
        // Positive control: an unconditional DE EOP certifies the later wait on its address.
        const auto w = Walk(Cat({EopTo(0x1000), WaitFor(0x1000, 1)}));
        Check(w.blockers.empty() && w.ranges.size() == 2, "walk: EOP then WAIT on it passes");
    }
    {
        const auto w = Walk({Type3(Eop, 2), 0x504, 0x1000});
        Check(Blocked(w, "holds 3 dwords"), "walk: truncated EOP is a blocker");
    }
    {
        const auto w = Walk({Type3(WaitRegMem, 3), 0x13, 0x1000, 0});
        Check(Blocked(w, "packet 0x3c holds"), "walk: truncated WAIT_REG_MEM is a blocker");
    }
    {
        const auto w = Walk({Type3(0x9F, 1), 0});
        Check(Blocked(w, "packet 0x9f is not on"), "walk: unknown opcode is a blocker");
    }
    {
        const auto w = Walk({Type3(WriteData, 5), 5u << 8, 0xFFFFFFFCu, 0xFFFFFFFFu, 1, 2});
        Check(Blocked(w, "wraps"), "walk: WRITE_DATA range that wraps is a blocker");
    }
    {
        // COND_EXEC whose window holds the EOP: that write may not run, so the wait is open.
        const auto eop = EopTo(0x1000);
        const auto w = Walk(Cat({{Type3(CondExec, 3), 0x2000 >> 2, 0, u32(eop.size())}, eop,
                                 WaitFor(0x1000, 1)}));
        Check(Blocked(w, "WAIT_REG_MEM polls 0x1000"), "walk: conditional producer does not certify");
        Check(std::ranges::any_of(w.ranges, [](const auto& r) { return r.address == 0x1000 && r.opcode == Eop; }),
              "walk: the conditional write is still reset");
    }
    {
        // After the COND_EXEC window ends, an unconditional write certifies again.
        const auto w = Walk(Cat({{Type3(CondExec, 3), 0x2000 >> 2, 0, 1}, {Type2}, EopTo(0x1000),
                                 WaitFor(0x1000, 1)}));
        Check(w.blockers.empty(), "walk: write after the COND_EXEC window certifies");
    }
    {
        // A constant-engine write runs concurrently with the DE: it does not certify a DE wait.
        const auto w = Walk(WaitFor(0x1000, 1), {Type3(DumpConstRam, 4), 0, 1, 0x1000, 0});
        Check(Blocked(w, "WAIT_REG_MEM polls 0x1000"), "walk: CE producer does not certify");
    }
    {
        const u32 control = u32(AmdGpu::EventType::PixelPipeStatDump) |
                            (u32(AmdGpu::EventIndex::ZpassDone) << 8);
        Check(Blocked(Walk({Type3(0x46, 1), control}), "lacks its two address dwords"),
              "walk: two-dword PixelPipeStatDump refuses missing address");
        Check(Blocked(Walk({Type3(0x46, 2), control, 0x1000}),
                      "lacks its two address dwords"),
              "walk: three-dword PixelPipeStatDump refuses missing address");
        Check(Blocked(Walk({Type3(0x46, 3), control, 0x1000, 0}),
                      "uncaptured Liverpool pixel_counter"),
              "walk: complete PixelPipeStatDump refuses uncaptured state");
    }
    {
        const auto w = Walk({Type3(CondExec, 3), 0x2000 >> 2, 0, 4,
                             Type3(0x11, 3), 1, 0x3000, 0});
        Check(Blocked(w, "conditional or CE SET_BASE"),
              "walk: conditional indirect base cannot authenticate later draw");
    }
    {
        std::map<int, int> m;
        const bool first = BbReplay::InsertUnique(m, 1, 10);
        const bool second = BbReplay::InsertUnique(m, 1, 20);
        Check(first && !second && m.size() == 1 && m.at(1) == 10,
              "image round trip: a duplicate key is refused, not dropped");
    }
    {
        ReplayFormat::HostObjectRecord object{0x1000, 0x80, 0x20, "semantic range"};
        Check(BbReplay::HostMatches(object, 0x1020, 8, 0x1000, 0x80),
              "host reset: named range accepts exact address/size/offset");
        Check(!BbReplay::HostMatches(object, 0x1020, 8, 0x2000, 0x80),
              "host reset: same name moved across builds rejects");
        Check(!BbReplay::HostMatches(object, 0x1020, 8, 0x1000, 0x81),
              "host reset: changed range size rejects");
        Check(!BbReplay::HostSpanValid(object, 0x1024, 8),
              "host reset: embedded target must match registered base plus offset");
        Check(!BbReplay::HostSpanValid(object, 0x1020, 0x61),
              "host reset: whole payload must fit");
        object.start=UINT64_MAX-0x10;
        Check(!BbReplay::HostSpanValid(object, UINT64_MAX-1, 8),
              "host reset: wrapping registered range rejects");
        object={0x1000, 0x80, 0x20, ""};
        Check(!BbReplay::HostSpanValid(object, 0x1020, 8), "host reset: empty name rejects");
        std::memset(object.name, 'x', sizeof(object.name));
        Check(!BbReplay::HostSpanValid(object, 0x1020, 8), "host reset: unterminated name rejects");
    }
    std::fprintf(stderr, "%s: %u failures\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
