// SPDX-License-Identifier: GPL-2.0-or-later
// Replay capture inventory (replay_capture.h): the environment parser accepts decimal digits
// within the bound only, and the PM4 census counts whole packets only, stopping a region (and
// marking the census incomplete) at an invalid header or a packet past the region's end.
#include <cstdio>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "video_core/amdgpu/replay_capture.h"

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

Pm4Census Scan(const std::vector<u32>& words) {
    Pm4Census census;
    CountRegion(census, words);
    return census;
}

} // namespace

int main() {
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
    std::fprintf(stderr, "%s: %u failures\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
