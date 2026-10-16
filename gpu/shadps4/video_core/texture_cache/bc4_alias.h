// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <initializer_list>

#include "video_core/amdgpu/resource.h"
#include "video_core/texture_cache/image_info.h"

namespace VideoCore::Bc4Alias {

constexpr bool AuthorityCompatible(bool gpu_modified, bool backing_dirty) {
    return !gpu_modified || !backing_dirty;
}

constexpr bool CompleteSubresource(u32 level, u32 layer, u32 levels, u32 layers) {
    return level == 0 && layer == 0 && levels == 1 && layers == 1;
}

inline bool CrossesBc4(const ImageInfo& a, const ImageInfo& b) {
    return (a.pixel_format == vk::Format::eBc4UnormBlock && !b.props.is_block) ||
           (b.pixel_format == vk::Format::eBc4UnormBlock && !a.props.is_block);
}

// A deliberately complete, unpadded block grid. No min-mip/min-layer copy or assumption that
// a storage shader overwrites all blocks is sufficient to admit a different guest layout.
inline const char* RejectLayout(const ImageInfo& a, const ImageInfo& b) {
    const auto& bc = a.pixel_format == vk::Format::eBc4UnormBlock ? a : b;
    const auto& ui = a.pixel_format == vk::Format::eBc4UnormBlock ? b : a;
    if (bc.pixel_format != vk::Format::eBc4UnormBlock ||
        ui.pixel_format != vk::Format::eR32G32Uint || !bc.props.is_block || ui.props.is_block)
        return "requires BC4Unorm and R32G32Uint";
    if (!bc.guest_address || bc.guest_address != ui.guest_address)
        return "different base addresses";
    for (const auto* info : {&bc, &ui}) {
        if (info->type != AmdGpu::ImageType::Color2D || info->resources.levels != 1 ||
            info->resources.layers != 1 || info->num_samples != 1 || info->size.depth != 1)
            return "requires one 2D mip, layer and sample";
        if (info->num_bits != 64 || info->props.is_volume || info->props.is_depth ||
            info->props.has_stencil || info->stencil_addr || info->stencil_size ||
            info->meta_info.cmask_addr || info->meta_info.fmask_addr || info->meta_info.htile_addr)
            return "unsupported aspect, bits or metadata";
        const bool linear = info->tile_mode == AmdGpu::TileMode::DisplayLinearAligned &&
                            info->array_mode == AmdGpu::ArrayMode::ArrayLinearAligned &&
                            !info->props.is_tiled;
        const bool micro = info->tile_mode == AmdGpu::TileMode::Thin1DThin &&
                           info->array_mode == AmdGpu::ArrayMode::Array1DTiledThin1 &&
                           info->props.is_tiled;
        if ((!linear && !micro) || info->bank_swizzle || info->alt_tile)
            return "unsupported tiling or swizzle";
    }
    if (bc.tile_mode != ui.tile_mode || bc.array_mode != ui.array_mode ||
        bc.props.is_pow2 != ui.props.is_pow2)
        return "different tiling or power-of-two policy";
    if (!bc.size.width || !bc.size.height || bc.size.width % 4 || bc.size.height % 4 ||
        bc.size.width / 4 != ui.size.width || bc.size.height / 4 != ui.size.height ||
        bc.pitch != bc.size.width || ui.pitch != ui.size.width)
        return "not an exact visible block grid";
    if (ui.props.is_tiled && (ui.size.width % 8 || ui.size.height % 8))
        return "incomplete microtiles";
    const u64 bytes = u64(ui.size.width) * ui.size.height * 8;
    if (!bytes || bytes != bc.guest_size || bytes != ui.guest_size)
        return "guest padding or differing byte coverage";
    for (const auto* info : {&bc, &ui}) {
        const auto& mip = info->mips_layout[0];
        if (mip.offset || mip.size != bytes || mip.pitch != info->size.width ||
            mip.height != info->size.height)
            return "mip padding or differing byte layout";
    }
    return nullptr;
}

inline const char* RejectStoredLayout(const ImageInfo& info) {
    auto other = info;
    if (info.pixel_format == vk::Format::eBc4UnormBlock) {
        other.pixel_format = vk::Format::eR32G32Uint;
        other.props.is_block = false;
        other.size.width /= 4;
        other.size.height /= 4;
        other.pitch /= 4;
        other.mips_layout[0].pitch /= 4;
        other.mips_layout[0].height /= 4;
    } else if (info.pixel_format == vk::Format::eR32G32Uint) {
        if (info.size.width > UINT32_MAX / 4 || info.size.height > UINT32_MAX / 4 ||
            info.pitch > UINT32_MAX / 4 || info.mips_layout[0].pitch > UINT32_MAX / 4 ||
            info.mips_layout[0].height > UINT32_MAX / 4)
            return "block dimensions overflow";
        other.pixel_format = vk::Format::eBc4UnormBlock;
        other.props.is_block = true;
        other.size.width *= 4;
        other.size.height *= 4;
        other.pitch *= 4;
        other.mips_layout[0].pitch *= 4;
        other.mips_layout[0].height *= 4;
    } else {
        return "not a converted BC4/integer image";
    }
    return RejectLayout(info, other);
}

} // namespace VideoCore::Bc4Alias
