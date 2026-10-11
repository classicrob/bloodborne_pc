// SPDX-License-Identifier: GPL-2.0-or-later
// bbport (Apple): the perf replay's image capture and restore (docs/macos/capture-replay.md,
// Verifier option A). At the quiescent cut every GPU-modified image is copied out of its Vulkan
// image with the texture cache's state for it; the replay recreates it at the same key and
// copies the texels back. Exact for the host state by construction: no tiling and no guest
// metadata encoding is involved.
//
// The state blob is opaque to the serializer, versioned, and only valid for the same build
// (it holds ImageInfo as bytes): a different layout is refused, never reinterpreted.
#pragma once

#include <algorithm>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <vector>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include <vulkan/vulkan_format_traits.hpp>
#include "video_core/texture_cache/image_info.h"

namespace VideoCore {

constexpr u32 CapturedImageVersion = 1;

/// One aspect of one mip level, every layer, tightly packed (layer-major).
struct CapturedAspect {
    u32 aspect; ///< vk::ImageAspectFlagBits: color, depth or stencil
    u32 mip;
    u32 layers;
    u32 width, height, depth;
    u32 texel_bytes; ///< of this aspect's buffer copy format
    std::vector<u8> texels;
};

struct CapturedImage {
    u32 version = CapturedImageVersion;
    std::vector<u8> state; ///< CapturedImageState as bytes
    std::vector<CapturedAspect> aspects;
};

/// The texture cache's metadata surfaces (CMASK / FMASK / HTILE address -> type, clear mask).
struct CapturedMetadata {
    u32 version = CapturedImageVersion;
    std::vector<u8> state; ///< CapturedMetadataEntry array as bytes
};

static_assert(std::is_trivially_copyable_v<ImageInfo>);

/// What the texture cache knows about one image besides its texels.
struct CapturedImageState {
    static constexpr u32 Magic = 0x53494242; ///< "BBIS"
    u32 magic = Magic;
    u32 version = CapturedImageVersion;
    u32 info_bytes = sizeof(ImageInfo); ///< a layout check: same build only
    u32 flags = 0;                      ///< ImageFlagBits, Registered and Picked cleared
    u32 usage = 0;  ///< texture | storage << 1 | render_target << 2 | depth << 3 | vo << 4
    u32 aspect_mask = 0;
    u32 num_mip_hashes = 0;
    u32 tracked = 0; ///< CPU write tracking was on for the whole image
    u64 image_uid = 0;
    u64 depth_uid = 0; ///< image_uid of the associated depth image, 0 if none
    u64 hash = 0;
    u64 mip_hashes[16]{};
    ImageInfo info{};
};
static_assert(std::is_trivially_copyable_v<CapturedImageState>);

struct CapturedMetadataEntry {
    VAddr address;
    u32 type; ///< TextureCache::MetaType
    s32 clear_mask;
};

inline std::vector<u8> EncodeState(const CapturedImageState& state) {
    std::vector<u8> bytes(sizeof(state));
    std::memcpy(bytes.data(), &state, sizeof(state));
    return bytes;
}

/// The state, or why it is refused.
inline std::optional<CapturedImageState> DecodeState(std::span<const u8> bytes,
                                                     std::string& why) {
    CapturedImageState state;
    if (bytes.size() != sizeof(state)) {
        why = "state blob of " + std::to_string(bytes.size()) + " bytes, " +
              std::to_string(sizeof(state)) + " expected (another build)";
        return std::nullopt;
    }
    std::memcpy(&state, bytes.data(), sizeof(state));
    if (state.magic != CapturedImageState::Magic || state.version != CapturedImageVersion ||
        state.info_bytes != sizeof(ImageInfo) || state.num_mip_hashes > 16) {
        why = "state blob magic, version or layout mismatch (another build)";
        return std::nullopt;
    }
    return state;
}

/// Bytes per texel of one aspect's buffer copy (Vulkan's packed buffer layouts), or 0 when this
/// capture does not handle the format (block-compressed, multi-planar, unknown depth formats).
inline u32 AspectTexelBytes(vk::Format format, vk::ImageAspectFlagBits aspect) {
    switch (aspect) {
    case vk::ImageAspectFlagBits::eColor:
        if (vk::blockExtent(format)[0] != 1 || vk::blockExtent(format)[1] != 1 ||
            vk::planeCount(format) != 1) {
            return 0;
        }
        return vk::blockSize(format);
    case vk::ImageAspectFlagBits::eDepth:
        switch (format) {
        case vk::Format::eD16Unorm:
        case vk::Format::eD16UnormS8Uint:
            return 2;
        case vk::Format::eD32Sfloat:
        case vk::Format::eD32SfloatS8Uint:
        case vk::Format::eX8D24UnormPack32:
        case vk::Format::eD24UnormS8Uint:
            return 4;
        default:
            return 0;
        }
    case vk::ImageAspectFlagBits::eStencil:
        switch (format) {
        case vk::Format::eS8Uint:
        case vk::Format::eD16UnormS8Uint:
        case vk::Format::eD24UnormS8Uint:
        case vk::Format::eD32SfloatS8Uint:
            return 1;
        default:
            return 0;
        }
    default:
        return 0;
    }
}

/// The aspects of `mask` one at a time, in a fixed order.
inline std::vector<vk::ImageAspectFlagBits> SplitAspects(vk::ImageAspectFlags mask) {
    std::vector<vk::ImageAspectFlagBits> out;
    for (const auto bit : {vk::ImageAspectFlagBits::eColor, vk::ImageAspectFlagBits::eDepth,
                           vk::ImageAspectFlagBits::eStencil}) {
        if (mask & bit) {
            out.push_back(bit);
        }
    }
    return out;
}

/// The extent of mip `mip` (volume images shrink in depth too).
inline vk::Extent3D MipExtent(const ImageInfo& info, u32 mip) {
    return {std::max(info.size.width >> mip, 1u), std::max(info.size.height >> mip, 1u),
            info.props.is_volume ? std::max(info.size.depth >> mip, 1u) : 1u};
}

/// The bytes of one aspect record for `info`, or 0 on overflow past 4 GiB.
inline u64 AspectBytes(const ImageInfo& info, u32 mip, u32 texel_bytes) {
    const auto extent = MipExtent(info, mip);
    const u64 bytes = u64(extent.width) * extent.height * extent.depth * texel_bytes *
                      info.resources.layers;
    return bytes >> 32 ? 0 : bytes;
}

} // namespace VideoCore
