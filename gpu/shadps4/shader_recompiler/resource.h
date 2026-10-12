// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"
#include "shader_recompiler/ir/type.h"
#include "video_core/amdgpu/resource.h"

#include <cstddef>
#include <boost/container/static_vector.hpp>

namespace Shader {

static constexpr u32 NUM_USER_DATA_REGS = 16;
// User-data registers of ALL stages of a pipeline share one push block, each stage taking its
// ud_mask.NumRegs() slots in stage order. Apple's inline vertex fetch and guard prologues read
// more user SGPRs than native shaders, so VS+FS can exceed 16; Apple sizes the block for four
// full stages (VS, TCS, TES, FS: geometry stages are a hard error there). Native keeps 16.
#ifdef __APPLE__
static constexpr u32 NUM_PUSH_USER_DATA_REGS = 4 * NUM_USER_DATA_REGS;
#else
static constexpr u32 NUM_PUSH_USER_DATA_REGS = NUM_USER_DATA_REGS;
#endif
static_assert(NUM_PUSH_USER_DATA_REGS % 4 == 0, "user data is declared as uvec4 members");
static constexpr u32 NUM_IMAGES = 64;
static constexpr u32 NUM_BUFFERS = 40;
static constexpr u32 NUM_SAMPLERS = 16;
static constexpr u32 NUM_FMASKS = 8;

using SharpLocation = u16;

constexpr SharpLocation UNKNOWN_LOCATION = std::numeric_limits<u16>::max();

template <typename T>
struct SharpFetch {
    static constexpr std::size_t N = sizeof(T) / sizeof(u32);
    static_assert(N <= 8);

    std::array<u32, N> immediates;
    std::array<SharpLocation, N> offsets;
    u8 load_mask;

    bool operator==(const SharpFetch&) const = default;

    template <u32 num_dwords = N>
        requires(num_dwords <= N)
    constexpr bool Fetch(const u32* flatbuf, T* out) const {
        u8 mask = load_mask;
        for (u32 i = 0; i < num_dwords; i++) {
            if (offsets[i] == UNKNOWN_LOCATION) {
                return false;
            }
        }
        std::array<u32, num_dwords> out_dw;
        for (u32 i = 0; i < num_dwords; i++) {
            out_dw[i] = (mask & 1) ? flatbuf[offsets[i]] : immediates[i];
            mask >>= 1;
        }
        std::memcpy(out, out_dw.data(), sizeof(out_dw));
        return true;
    }
};

enum class SharpFetchPostOp : u8 {
    None,
    // For buffers
    BitwiseOrDw1WithImm,
    OffsetByProgramBase,
    // For images,
    ConvertCubeTo2DArray,
    // For samplers
    DisableAnisoIfSingleLod,
    ForceRepeatXyzClamp,
    ForceLastTexelXyClamp,
    ClearAnisoRatioAndThreshold,
};

enum class BufferType : u8 {
    Guest,
    Flatbuf,
    BdaPagetable,
    FaultBuffer,
    GdsBuffer,
    SharedMemory,
    ClipPlanes,
    /// Geometry emulation (GsEmulationRole): the draw's scratch (header, ESGS and GSVS records,
    /// strip index runs) and the guest index buffer an EsCompute shader pulls vertex ids from.
    GsScratch,
    GsIndexInput,
};

struct BufferResource {
    SharpFetch<AmdGpu::Buffer> sharp_fetch{};
    IR::Type used_types{};
    BufferType buffer_type{};
    bool is_written{};
    bool is_formatted{};
    SharpFetchPostOp post_op{};
    u32 post_op_dw1_mask{};
    // Structured accesses without index bounds may need bytes past the descriptor size.
    u64 required_binding_bytes{};
    /// bbport (Apple): the stream-out buffer a CopyStreamOut kernel's stores through this
    /// descriptor write (their binding is checked against its VGT size at record time), or
    /// NoStreamOut.
    static constexpr u8 NoStreamOut = 0xFF;
    u8 stream_out_buffer = NoStreamOut;

    bool IsSpecial() const noexcept {
        return buffer_type != BufferType::Guest;
    }

    constexpr AmdGpu::Buffer GetSharp(const auto& info) const noexcept {
        AmdGpu::Buffer buffer;
        if (!sharp_fetch.Fetch(info.FlatUserData().data(), &buffer)) {
            return AmdGpu::Buffer::Null();
        }
        if (post_op == SharpFetchPostOp::BitwiseOrDw1WithImm) {
            reinterpret_cast<u32*>(&buffer)[1] |= post_op_dw1_mask;
        } else if (post_op == SharpFetchPostOp::OffsetByProgramBase) {
            buffer.base_address += info.ProgramBase();
        }
        if (!buffer.Valid()) {
            return AmdGpu::Buffer::Null();
        }
        return buffer;
    }
};
using BufferResourceList = boost::container::static_vector<BufferResource, NUM_BUFFERS>;

enum class MipStorageFallbackMode : u16 {
    None,
    DynamicIndex,
    ConstantIndex,
};

struct ImageResource {
    SharpFetch<AmdGpu::Image> sharp_fetch{};
    bool is_depth{};
    bool is_atomic{};
    bool is_array{};
    bool is_written{};
    bool is_r128{};
    /// bbport: read other than by normalized sampling without offsets (texel loads, size
    /// queries, offsets): needs the native-size image, not a reduced scene proxy.
    bool needs_native{};
    u8 constant_mip_index{};
    MipStorageFallbackMode mip_fallback_mode{};
    SharpFetchPostOp post_op{};
    /// bbport: descriptor count of this image in the permutation it belongs to, from its
    /// specialization when the permutation is stored (0 before). Pipeline layouts and descriptor
    /// writes use it: a pipeline rebuilt from the stage cache has no live user data, and
    /// NumBindings (live) stays what selects the permutation.
    u32 permutation_bindings{};

    constexpr AmdGpu::Image GetSharp(const auto& info) const noexcept {
        AmdGpu::Image image{};
        if (!Fetch(info.FlatUserData().data(), &image)) {
            return AmdGpu::Image::Null(is_depth);
        }
        if (post_op == SharpFetchPostOp::ConvertCubeTo2DArray) {
            image.type = u64(AmdGpu::ImageType::Color2DArray);
            image.depth = (image.depth + 1) * 6 - 1;
        }
        if (!image.Valid()) {
            image = AmdGpu::Image::Null(is_depth);
        } else if (is_depth) {
            const auto data_fmt = image.GetDataFmt();
            if (data_fmt != AmdGpu::DataFormat::Format16 &&
                data_fmt != AmdGpu::DataFormat::Format32) {
                image = AmdGpu::Image::Null(true);
            }
        }
        return image;
    }

    constexpr bool Fetch(const u32* flatbuf, AmdGpu::Image* out) const {
        if (!is_r128) {
            // Fetch full 8 byte T#
            return sharp_fetch.Fetch(flatbuf, out);
        }
        // Fetch r128 T# and fix pitch
        if (!sharp_fetch.Fetch<4>(flatbuf, out)) {
            return false;
        }
        out->pitch = out->width;
        return true;
    }

    u32 NumBindings(const auto& info) const {
        const AmdGpu::Image tsharp = GetSharp(info);
        return (mip_fallback_mode == MipStorageFallbackMode::DynamicIndex)
                   ? (tsharp.last_level - tsharp.base_level + 1)
                   : 1;
    }
};
using ImageResourceList = boost::container::static_vector<ImageResource, NUM_IMAGES>;

struct SamplerResource {
    SharpFetch<AmdGpu::Sampler> sharp_fetch{};
    SharpFetchPostOp post_op{};
    SharpLocation post_op_tsharp_dw3_off{};
    bool is_depth{};

    constexpr AmdGpu::Sampler GetSharp(const auto& info) const noexcept {
        AmdGpu::Sampler sampler{};
        sharp_fetch.Fetch(info.FlatUserData().data(), &sampler);
        if (post_op == SharpFetchPostOp::DisableAnisoIfSingleLod) {
            const u32 tsharp_dw3 = info.FlatUserData()[post_op_tsharp_dw3_off];
            if (((tsharp_dw3 >> 12) & 0xff) == 0) {
                sampler.max_aniso.Assign(AmdGpu::AnisoRatio::One);
            }
        } else if (post_op == SharpFetchPostOp::ForceRepeatXyzClamp) {
            sampler.clamp_x.Assign(AmdGpu::ClampMode::Wrap);
            sampler.clamp_y.Assign(AmdGpu::ClampMode::Wrap);
            sampler.clamp_z.Assign(AmdGpu::ClampMode::Wrap);
        } else if (post_op == SharpFetchPostOp::ForceLastTexelXyClamp) {
            sampler.clamp_x.Assign(AmdGpu::ClampMode::ClampLastTexel);
            sampler.clamp_y.Assign(AmdGpu::ClampMode::ClampLastTexel);
            sampler.clamp_z.Assign(AmdGpu::ClampMode::Wrap);
        } else if (post_op == SharpFetchPostOp::ClearAnisoRatioAndThreshold) {
            sampler.max_aniso.Assign(AmdGpu::AnisoRatio::One);
            sampler.aniso_threshold.Assign(0);
        }
        return sampler;
    }
};
using SamplerResourceList = boost::container::static_vector<SamplerResource, NUM_SAMPLERS>;

struct FMaskResource {
    SharpLocation sharp_idx;

    constexpr AmdGpu::Image GetSharp(const auto& info) const noexcept {
        return info.template ReadUdSharp<AmdGpu::Image>(sharp_idx);
    }
};
using FMaskResourceList = boost::container::static_vector<FMaskResource, NUM_FMASKS>;

struct PushData {
    // SPIR-V member indices (DefinePushDataBlock): four floats, ud_regs as uvec4 members,
    // buf_offsets as two uvec4 and one uvec2, then motion_param and scene_size.
    static constexpr u32 XOffsetIndex = 0;
    static constexpr u32 YOffsetIndex = 1;
    static constexpr u32 XScaleIndex = 2;
    static constexpr u32 YScaleIndex = 3;
    static constexpr u32 UdRegsIndex = 4;
    static constexpr u32 BufOffsetIndex = UdRegsIndex + NUM_PUSH_USER_DATA_REGS / 4;
    static constexpr u32 MotionParamIndex = BufOffsetIndex + NUM_BUFFERS / 16 + 1;
    static constexpr u32 SceneSizeIndex = MotionParamIndex + 1;
    // Byte offsets of those members.
    static constexpr u32 UdRegsOffset = 16;
    static constexpr u32 BufOffsetsOffset = UdRegsOffset + 4 * NUM_PUSH_USER_DATA_REGS;
    static constexpr u32 MotionParamOffset = BufOffsetsOffset + NUM_BUFFERS;
    static constexpr u32 SceneSizeOffset = MotionParamOffset + 4;
#ifdef __APPLE__
    // Depth bounds emulation (HwFragmentRuntimeInfo::depth_bounds_test): DB_DEPTH_BOUNDS_MIN/MAX.
    static constexpr u32 DepthBoundsMinIndex = SceneSizeIndex + 1;
    static constexpr u32 DepthBoundsMaxIndex = SceneSizeIndex + 2;
    static constexpr u32 DepthBoundsMinOffset = SceneSizeOffset + 4;
    static constexpr u32 DepthBoundsMaxOffset = SceneSizeOffset + 8;
#endif

    float xoffset;
    float yoffset;
    float xscale;
    float yscale;
    std::array<u32, NUM_PUSH_USER_DATA_REGS> ud_regs;
    std::array<u8, NUM_BUFFERS> buf_offsets;
    u32 motion_param; ///< bbport: object motion parameter index (0: off)
    u32 scene_size; ///< reduced width | height << 16; zero means native pixel coordinates
#ifdef __APPLE__
    float depth_bounds_min; ///< inclusive bounds on the stored depth (depth_bounds_test)
    float depth_bounds_max;
#endif

    void AddOffset(u32 binding, u32 offset) {
        ASSERT(offset < 256 && binding < buf_offsets.size());
        buf_offsets[binding] = offset;
    }
};
static_assert(offsetof(PushData, ud_regs) == PushData::UdRegsOffset);
static_assert(offsetof(PushData, buf_offsets) == PushData::BufOffsetsOffset);
static_assert(offsetof(PushData, motion_param) == PushData::MotionParamOffset);
static_assert(offsetof(PushData, scene_size) == PushData::SceneSizeOffset);
static_assert(NUM_BUFFERS == 2 * 16 + 8, "buf_offsets is declared as two uvec4 and one uvec2");
#ifdef __APPLE__
// Above Vulkan's guaranteed 128 bytes: Instance checks maxPushConstantsSize (4096 on M4).
static_assert(offsetof(PushData, depth_bounds_min) == PushData::DepthBoundsMinOffset);
static_assert(offsetof(PushData, depth_bounds_max) == PushData::DepthBoundsMaxOffset);
static_assert(PushData::DepthBoundsMinOffset == 320 && sizeof(PushData) == 328);
#else
static_assert(sizeof(PushData) <= 128,
              "PushData size is greater than minimum size guaranteed by Vulkan spec");
static_assert(PushData::BufOffsetsOffset == 80 && PushData::SceneSizeOffset == 124,
              "native push layout must not change");
#endif

} // namespace Shader
