// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <initializer_list>

#include "video_core/amdgpu/resource.h"
#include "video_core/texture_cache/image_info.h"

namespace VideoCore::Bc4Alias {

constexpr bool RangesOverlap(VAddr a, u64 a_size, VAddr b, u64 b_size) {
    if (!a_size || !b_size) return false;
    return a <= b ? b - a < a_size : a - b < b_size;
}

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

inline bool NeedsAliasPreflight(const ImageInfo& request, const ImageInfo& cached,
                                bool cached_converted) {
    // Converted ownership must never escape through ordinary overlap resolution. For ordinary
    // images, an interior noninteger view is not the same-base integer block-grid alias route.
    return cached_converted ||
           (CrossesBc4(request, cached) &&
            (request.guest_address == cached.guest_address ||
             request.pixel_format == vk::Format::eR32G32Uint ||
             cached.pixel_format == vk::Format::eR32G32Uint));
}

// Prospective Stage-A hazard classification; no cache registry or guest reads.
inline bool IsFloatTailCandidate(const ImageInfo& info) {
    return info.guest_address && info.pixel_format == vk::Format::eR32G32Sfloat &&
           info.type == AmdGpu::ImageType::Color2D && info.size == Extent3D{1, 1, 1} &&
           info.resources.levels == 1 && info.resources.layers == 1 && info.num_samples == 1 &&
           info.num_bits == 64 && info.pitch == 8 && info.guest_size == 512 &&
           info.tile_mode == AmdGpu::TileMode::Thin1DThin &&
           info.array_mode == AmdGpu::ArrayMode::Array1DTiledThin1 && info.props.is_tiled &&
           !info.props.is_block && !info.props.is_volume && !info.props.is_depth &&
           !info.props.has_stencil && !info.bank_swizzle && !info.alt_tile &&
           !info.stencil_addr && !info.stencil_size && !info.meta_info.cmask_addr &&
           !info.meta_info.fmask_addr && !info.meta_info.htile_addr &&
           info.mips_layout[0].offset == 0 && info.mips_layout[0].size == 512 &&
           info.mips_layout[0].pitch == 8 && info.mips_layout[0].height == 8;
}

// The first route is one complete 64-bit block in an exact 2x1 BC mip. Padding has
// backing authority only; it is never uploaded from or published by the Float image.
inline const char* RejectFloatTail(const ImageInfo& child, const ImageInfo& parent, u32 mip) {
    if (!IsFloatTailCandidate(child)) return "unsupported Float tail descriptor";
    if (parent.pixel_format != vk::Format::eBc4UnormBlock || !parent.props.is_block ||
        parent.type != AmdGpu::ImageType::Color2D || parent.resources.layers != 1 ||
        parent.resources.levels < 2 || parent.resources.levels > parent.mips_layout.size() ||
        parent.num_samples != 1 || parent.num_bits != 64 || parent.size.depth != 1 ||
        parent.tile_mode != AmdGpu::TileMode::Thin1DThin ||
        parent.array_mode != AmdGpu::ArrayMode::Array1DTiledThin1 || !parent.props.is_tiled ||
        parent.props.is_volume || parent.props.is_depth || parent.props.has_stencil ||
        parent.stencil_addr || parent.stencil_size || parent.meta_info.cmask_addr ||
        parent.meta_info.fmask_addr || parent.meta_info.htile_addr || parent.bank_swizzle ||
        parent.alt_tile || !parent.guest_address || mip >= parent.resources.levels)
        return "unsupported BC parent layout";
    if (std::max(parent.size.width >> mip, 1u) != 2 ||
        std::max(parent.size.height >> mip, 1u) != 1)
        return "requires exact 2x1 BC mip";
    const auto& tail = parent.mips_layout[mip];
    if (tail.size != 512 || tail.pitch != 32 || tail.height != 32 ||
        tail.offset > parent.guest_size || 512 > parent.guest_size - tail.offset ||
        child.guest_address < parent.guest_address ||
        child.guest_address - parent.guest_address != tail.offset ||
        child.MipOf(parent) != s32(mip) || child.SliceOf(parent, mip) != 0)
        return "not the exact first block of the selected tail allocation";
    return nullptr;
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
