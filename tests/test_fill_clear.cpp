// SPDX-License-Identifier: GPL-2.0-or-later
// When the SDK fill kernel 0x8b355b5a may be replaced by a buffer fill and an image clear
// (fill_clear.h): standalone CPU checks, no game, Vulkan device or window required.
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <limits>
#include "gpu/shadps4/video_core/renderer_vulkan/fill_clear.h"

using namespace Vulkan::FillClear;

namespace {
// The census shape: 2560x1440 RGBA16F, 3768320 records, 29440 groups of 64 threads.
Dispatch GoodDispatch() {
    return {.threads_x = 64, .threads_x_partial = 0, .threads_y = 1, .threads_z = 1,
            .dim_x = 29440, .dim_y = 1, .dim_z = 1, .start_x = 0, .start_y = 0, .start_z = 0,
            .user_sgprs = 12, .tgid_enable = 1};
}
Target GoodTarget(uint32_t stride = 8, uint32_t records = 3768320) {
    return {.stride = stride, .num_records = records, .swizzle_enable = false,
            .add_tid_enable = false, .dst_sel_x = 4, .dst_sel_y = 5, .dst_sel_z = 6,
            .dst_sel_w = 7};
}
uint32_t Word(const std::array<uint8_t, 8>& texel, uint32_t at = 0) {
    uint32_t word;
    std::memcpy(&word, texel.data() + at, 4);
    return word;
}
} // namespace

int main() {
    // Raw formats only: 11_11_10 (raw 7, which the remapped format reads as 10_11_11 with R and
    // B swapped) and other number formats are refused.
    assert(TargetFormat(12, 7) == Format::Rgba16Float);
    assert(TargetFormat(6, 7) == Format::B10G11R11Ufloat);
    assert(TargetFormat(10, 0) == Format::Rgba8Unorm);
    assert(!TargetFormat(7, 7));  // 11_11_10
    assert(!TargetFormat(10, 1)); // 8_8_8_8 snorm
    assert(!TargetFormat(10, 4)); // 8_8_8_8 uint
    assert(!TargetFormat(12, 5)); // 16_16_16_16 sint
    assert(!TargetFormat(5, 7));  // 16_16 float (the 32 KB fills)

    // Coverage: the census shape covers every record, exactly.
    assert(CoversAllElements(GoodDispatch(), GoodTarget(), 8));
    {
        auto d = GoodDispatch();
        d.dim_x = 29439; // one group short: the last 128 records are not stored
        assert(!CoversAllElements(d, GoodTarget(), 8));
    }
    // The SGPR ABI: s12 must be the workgroup id x (12 user SGPRs, TGID_X enabled).
    for (const uint32_t user_sgprs : {0u, 11u, 13u, 16u}) {
        auto d = GoodDispatch();
        d.user_sgprs = user_sgprs;
        assert(!CoversAllElements(d, GoodTarget(), 8));
    }
    {
        auto d = GoodDispatch();
        d.tgid_enable = 2; // TGID_Y only: s12 is not the group id x
        assert(!CoversAllElements(d, GoodTarget(), 8));
    }
    // Group shape and dispatch offsets.
    for (const auto field : {&Dispatch::threads_x, &Dispatch::threads_y, &Dispatch::threads_z,
                             &Dispatch::dim_y, &Dispatch::dim_z}) {
        auto d = GoodDispatch();
        d.*field += 1;
        assert(!CoversAllElements(d, GoodTarget(), 8));
    }
    for (const auto field : {&Dispatch::threads_x_partial, &Dispatch::start_x, &Dispatch::start_y,
                             &Dispatch::start_z}) {
        auto d = GoodDispatch();
        d.*field = 1;
        assert(!CoversAllElements(d, GoodTarget(), 8));
    }
    // The 32-bit element index must not wrap.
    {
        auto d = GoodDispatch();
        d.dim_x = (1u << 25) + 1;
        assert(!CoversAllElements(d, GoodTarget(8, 1000), 8));
        d.dim_x = 1u << 25;
        assert(CoversAllElements(d, GoodTarget(8, std::numeric_limits<uint32_t>::max()), 8));
    }
    // The V#: stride is the texel, no swizzle or add_tid, identity dst_sel, records.
    assert(!CoversAllElements(GoodDispatch(), GoodTarget(4), 8));
    assert(!CoversAllElements(GoodDispatch(), GoodTarget(8, 0), 8));
    {
        auto t = GoodTarget();
        t.swizzle_enable = true;
        assert(!CoversAllElements(GoodDispatch(), t, 8));
        t = GoodTarget();
        t.add_tid_enable = true;
        assert(!CoversAllElements(GoodDispatch(), t, 8));
        t = GoodTarget();
        std::swap(t.dst_sel_x, t.dst_sel_z); // R and B swapped
        assert(!CoversAllElements(GoodDispatch(), t, 8));
    }

    // Values: only exactly representable components, so no rounding mode is involved.
    assert(SmallFloatBits(1.0f, 10, true) == 0x3c00u);
    assert(SmallFloatBits(-2.0f, 10, true) == 0xc000u);
    assert(SmallFloatBits(65504.0f, 10, true) == 0x7bffu);
    assert(!SmallFloatBits(65536.0f, 10, true));      // overflow
    assert(!SmallFloatBits(0.1f, 10, true));          // rounding
    assert(!SmallFloatBits(-0.0f, 10, true));         // -0
    assert(!SmallFloatBits(1.0e-6f, 10, true));       // a half denormal
    assert(!SmallFloatBits(std::numeric_limits<float>::infinity(), 10, true));
    assert(!SmallFloatBits(std::numeric_limits<float>::quiet_NaN(), 10, true));
    assert(!SmallFloatBits(-1.0f, 6, false));         // no sign in 11-bit floats
    assert(SmallFloatBits(1.0f, 6, false) == 0x3c0u); // 11-bit float 1.0
    assert(SmallFloatBits(1.0f, 5, false) == 0x1e0u); // 10-bit float 1.0
    assert(!SmallFloatBits(1.0f + 1.0f / 64, 5, false) && SmallFloatBits(1.0f + 1.0f / 64, 6, false));
    assert(Unorm8Bits(0.0f) == 0u && Unorm8Bits(1.0f) == 255u);
    assert(!Unorm8Bits(0.5f) && !Unorm8Bits(1.5f) && !Unorm8Bits(-0.0f));

    // Texels and the repeated word of every written byte (padding included).
    {
        const auto zero = Texel(Format::Rgba16Float, {0, 0, 0, 0});
        assert(zero && Pattern(Format::Rgba16Float, *zero) == 0u);
        const auto ones = Texel(Format::Rgba16Float, {1, 1, 1, 1});
        assert(ones && Pattern(Format::Rgba16Float, *ones) == 0x3c003c00u);
        // (1, 0, 0, 1): halves 0x00003c00 and 0x3c000000 differ, no single word.
        const auto red = Texel(Format::Rgba16Float, {1, 0, 0, 1});
        assert(red && Word(*red) == 0x00003c00u && Word(*red, 4) == 0x3c000000u);
        assert(!Pattern(Format::Rgba16Float, *red));
        assert(!Texel(Format::Rgba16Float, {0.1f, 0, 0, 0}));
    }
    {
        // 10_11_11: x in bits 0-10, y in 11-21, z in 22-31, w not stored. Unequal R and B land
        // in their own fields (an 11_11_10 target is refused above).
        const auto texel = Texel(Format::B10G11R11Ufloat, {1.0f, 0.0f, 2.0f, 0.25f});
        assert(texel);
        const uint32_t expected = 0x3c0u | (0u << 11) | ((16u << 5) << 22);
        assert(Word(*texel) == expected && Pattern(Format::B10G11R11Ufloat, *texel) == expected);
        assert(!Texel(Format::B10G11R11Ufloat, {-1.0f, 0, 0, 0}));
    }
    {
        const auto texel = Texel(Format::Rgba8Unorm, {1.0f, 0.0f, 1.0f, 1.0f});
        assert(texel && Pattern(Format::Rgba8Unorm, *texel) == 0xffff00ffu);
        assert(!Texel(Format::Rgba8Unorm, {0.5f, 0, 0, 0}));
    }
    std::printf("fill-clear-test: PASS\n");
    return 0;
}
