// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: when the SDK fill kernel 0x8b355b5a can be replaced by a fill of the same bytes and an
// exact clear of the render target they hold (vk_shader_hle.cpp FillAsImageClear). Pure: no
// device, tested by tests/test_fill_clear.cpp.
//
// The kernel (Opus-GS disassembly, out/shader_sweep/disasm-renderer/0x8b355b5a.s) loads one float4
// with S_BUFFER_LOAD_DWORDX4 from the V# at s[8:11] (offset 0) and BUFFER_STORE_FORMAT_XYZW's it
// (typed: converted by the V#'s format) to the V# at s[4:7], elements g*128 + (t & 63) and that
// +64, where t is the thread (v0) and g the workgroup id it reads from s12.
#pragma once

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>

namespace Vulkan::FillClear {

/// The image formats a fill is replaced for, by the raw (not remapped) V# format they store as.
enum class Format : uint8_t { Rgba16Float, B10G11R11Ufloat, Rgba8Unorm };

/// Raw V# data_format/num_format: 16_16_16_16 float, 10_11_11 float, 8_8_8_8 unorm. Anything else
/// (11_11_10, which the remapped format turns into 10_11_11 with R and B swapped, included) no.
inline std::optional<Format> TargetFormat(uint32_t raw_data_format, uint32_t raw_number_format) {
    if (raw_data_format == 12 && raw_number_format == 7) {
        return Format::Rgba16Float;
    }
    if (raw_data_format == 6 && raw_number_format == 7) {
        return Format::B10G11R11Ufloat;
    }
    if (raw_data_format == 10 && raw_number_format == 0) {
        return Format::Rgba8Unorm;
    }
    return std::nullopt;
}

inline uint32_t TexelBytes(Format format) {
    return format == Format::Rgba16Float ? 8 : 4;
}

/// The dispatch and the kernel's SGPR layout.
struct Dispatch {
    uint32_t threads_x, threads_x_partial, threads_y, threads_y_partial, threads_z,
        threads_z_partial;
    uint32_t dim_x, dim_y, dim_z;
    uint32_t start_x, start_y, start_z;
    uint32_t user_sgprs;  ///< COMPUTE_PGM_RSRC2 user SGPR count
    uint32_t tgid_enable; ///< COMPUTE_PGM_RSRC2 TGID_X/Y/Z enable bits
};

/// The target V#'s raw fields.
struct Target {
    uint32_t stride, num_records;
    bool swizzle_enable, add_tid_enable;
    uint32_t dst_sel_x, dst_sel_y, dst_sel_z, dst_sel_w;
};

/// Whether the kernel stores every element of the target, each exactly once, at element i of the
/// linear buffer: 64 threads per group, one group row, no start offset or partial group; s12 holds
/// the workgroup id x (12 user SGPRs, then TGID_X); an unswizzled V# with identity dst_sel and the
/// texel as its stride; enough groups for every record, no 32-bit wrap of the element index, and a
/// byte size a buffer binding takes (32 bits).
inline bool CoversAllElements(const Dispatch& d, const Target& t, uint32_t texel_bytes) {
    if (d.threads_x != 64 || d.threads_x_partial != 0 || d.threads_y != 1 ||
        d.threads_y_partial != 0 || d.threads_z != 1 || d.threads_z_partial != 0 ||
        d.dim_y != 1 || d.dim_z != 1 || d.start_x != 0 || d.start_y != 0 || d.start_z != 0) {
        return false;
    }
    if (d.user_sgprs != 12 || (d.tgid_enable & 1) == 0) {
        return false;
    }
    if (t.stride != texel_bytes || t.swizzle_enable || t.add_tid_enable || t.dst_sel_x != 4 ||
        t.dst_sel_y != 5 || t.dst_sel_z != 6 || t.dst_sel_w != 7 || t.num_records == 0) {
        return false;
    }
    const uint64_t elements = uint64_t(d.dim_x) * 128;
    const uint64_t bytes = uint64_t(t.num_records) * t.stride;
    return elements >= t.num_records && elements <= (uint64_t(1) << 32) && bytes <= UINT32_MAX;
}

/// A float exactly representable as a small float with a 5-bit exponent (bias 15): its bits, or
/// nothing (rounding, denormal, out of range, Inf/NaN, -0, or a sign the format has not).
inline std::optional<uint32_t> SmallFloatBits(float value, uint32_t mantissa_bits, bool has_sign) {
    const uint32_t bits = std::bit_cast<uint32_t>(value);
    if (bits == 0) {
        return 0u;
    }
    const uint32_t sign = bits >> 31;
    const int32_t exponent = int32_t((bits >> 23) & 0xff) - 127;
    const uint32_t mantissa = bits & 0x7fffff;
    if ((sign && !has_sign) || exponent < -14 || exponent > 15 ||
        (mantissa & ((1u << (23 - mantissa_bits)) - 1)) != 0) {
        return std::nullopt;
    }
    return (sign << (5 + mantissa_bits)) | (uint32_t(exponent + 15) << mantissa_bits) |
           (mantissa >> (23 - mantissa_bits));
}

/// A float in [0, 1] whose unorm8 value needs no rounding (v * 255 is an integer).
inline std::optional<uint32_t> Unorm8Bits(float value) {
    if (!(value >= 0.0f && value <= 1.0f) || std::bit_cast<uint32_t>(value) == 0x80000000u) {
        return std::nullopt;
    }
    const double scaled = double(value) * 255.0;
    if (scaled != std::floor(scaled)) {
        return std::nullopt;
    }
    return uint32_t(scaled);
}

/// The texel the typed store writes for `value` (TexelBytes of it), when no rounding is involved.
inline std::optional<std::array<uint8_t, 8>> Texel(Format format,
                                                   const std::array<float, 4>& value) {
    std::array<uint8_t, 8> texel{};
    if (format == Format::Rgba16Float) {
        for (uint32_t c = 0; c < 4; ++c) {
            const auto half = SmallFloatBits(value[c], 10, true);
            if (!half) {
                return std::nullopt;
            }
            const uint16_t bits = uint16_t(*half);
            std::memcpy(texel.data() + c * 2, &bits, 2);
        }
        return texel;
    }
    if (format == Format::B10G11R11Ufloat) {
        // x: bits 0-10, y: 11-21, z: 22-31 (10_11_11); w is not stored.
        const auto r = SmallFloatBits(value[0], 6, false);
        const auto g = SmallFloatBits(value[1], 6, false);
        const auto b = SmallFloatBits(value[2], 5, false);
        if (!r || !g || !b) {
            return std::nullopt;
        }
        const uint32_t packed = *r | (*g << 11) | (*b << 22);
        std::memcpy(texel.data(), &packed, 4);
        return texel;
    }
    for (uint32_t c = 0; c < 4; ++c) {
        const auto unorm = Unorm8Bits(value[c]);
        if (!unorm) {
            return std::nullopt;
        }
        texel[c] = uint8_t(*unorm);
    }
    return texel;
}

/// The 32-bit word whose repetition is every byte the kernel writes (every element holds the same
/// texel), when the texel is one repeated word; an 8-byte texel with unequal halves: nothing.
inline std::optional<uint32_t> Pattern(Format format, const std::array<uint8_t, 8>& texel) {
    uint32_t low = 0, high = 0;
    std::memcpy(&low, texel.data(), 4);
    std::memcpy(&high, texel.data() + 4, 4);
    if (TexelBytes(format) == 8 && low != high) {
        return std::nullopt;
    }
    return low;
}

} // namespace Vulkan::FillClear
