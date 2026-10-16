// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include "video_core/texture_cache/bc4_alias.h"

using namespace VideoCore;

void RunBc4AliasPolicy() {
    assert(Bc4Alias::RangesOverlap(0x100000, 2048, 0x100008, 8));
    assert(Bc4Alias::RangesOverlap(0x100008, 8, 0x100000, 2048));
    assert(Bc4Alias::RangesOverlap(0x0ffff8, 16, 0x100000, 2048));
    assert(!Bc4Alias::RangesOverlap(0x100800, 8, 0x100000, 2048));
    assert(!Bc4Alias::RangesOverlap(0x0ffff8, 8, 0x100000, 2048));
    assert(!Bc4Alias::RangesOverlap(0x100000, 0, 0x100000, 2048));
    assert(!Bc4Alias::RangesOverlap(0x100000, 2048, 0x100000, 0));
    assert(Bc4Alias::RangesOverlap(UINT64_MAX - 15, 16, UINT64_MAX - 7, 8));
    assert(!Bc4Alias::RangesOverlap(UINT64_MAX - 15, 16, 0, 8));
    assert(Bc4Alias::AuthorityCompatible(false, false));
    assert(Bc4Alias::AuthorityCompatible(false, true));
    assert(Bc4Alias::AuthorityCompatible(true, false));
    assert(!Bc4Alias::AuthorityCompatible(true, true));
    ImageInfo bc{};
    bc.pixel_format = vk::Format::eBc4UnormBlock;
    bc.props.is_block = true;
    bc.type = AmdGpu::ImageType::Color2D;
    bc.resources = {1, 1};
    bc.size = {64, 64, 1};
    bc.pitch = 64;
    bc.num_bits = 64;
    bc.guest_address = 0x100000;
    bc.guest_size = 2048;
    bc.mips_layout[0] = {2048, 64, 64, 0};
    auto ui = bc;
    ui.pixel_format = vk::Format::eR32G32Uint;
    ui.props.is_block = false;
    ui.size = {16, 16, 1};
    ui.pitch = 16;
    ui.mips_layout[0] = {2048, 16, 16, 0};
    assert(!Bc4Alias::RejectLayout(bc, ui));
    assert(!Bc4Alias::RejectLayout(ui, bc));
    assert(!Bc4Alias::RejectStoredLayout(bc));
    assert(!Bc4Alias::RejectStoredLayout(ui));
    assert(Bc4Alias::NeedsAliasPreflight(bc, ui, false));
    assert(Bc4Alias::NeedsAliasPreflight(ui, bc, false));
    assert(!Bc4Alias::NeedsAliasPreflight(bc, bc, false));
    assert(Bc4Alias::NeedsAliasPreflight(bc, bc, true));
    {
        // The observed sampled mip chain and an interior, unconverted float image.
        auto sampled = bc;
        sampled.guest_address = 0x70e2470700;
        sampled.guest_size = 0xab400;
        sampled.size = {1024, 1024, 1};
        sampled.pitch = 1024;
        sampled.resources.levels = 11;
        sampled.props.is_tiled = sampled.props.is_pow2 = true;
        sampled.tile_mode = AmdGpu::TileMode::Thin1DThin;
        sampled.array_mode = AmdGpu::ArrayMode::Array1DTiledThin1;
        auto interior = ui;
        interior.pixel_format = vk::Format::eR32G32Sfloat;
        interior.guest_address = 0x70e24c8800;
        interior.guest_size = 0x200;
        interior.size = {1, 1, 1};
        interior.pitch = 8;
        interior.props.is_tiled = true;
        interior.tile_mode = sampled.tile_mode;
        interior.array_mode = sampled.array_mode;
        assert(Bc4Alias::RangesOverlap(sampled.guest_address, sampled.guest_size,
                                      interior.guest_address, interior.guest_size));
        assert(!Bc4Alias::NeedsAliasPreflight(sampled, interior, false));
        assert(!Bc4Alias::NeedsAliasPreflight(interior, sampled, false));
        assert(Bc4Alias::NeedsAliasPreflight(sampled, interior, true));
        assert(Bc4Alias::NeedsAliasPreflight(interior, sampled, true));
        assert(Bc4Alias::NeedsAliasPreflight(interior, interior, true));
        // Classification changes no admission rule: this is never a supported conversion.
        assert(Bc4Alias::RejectLayout(sampled, interior));
        assert(Bc4Alias::RejectLayout(interior, sampled));
        auto same_base = interior;
        same_base.guest_address = sampled.guest_address;
        assert(Bc4Alias::NeedsAliasPreflight(sampled, same_base, false));
        assert(Bc4Alias::NeedsAliasPreflight(same_base, sampled, false));
        auto wrong_base_uint = interior;
        wrong_base_uint.pixel_format = vk::Format::eR32G32Uint;
        assert(Bc4Alias::NeedsAliasPreflight(sampled, wrong_base_uint, false));
        assert(Bc4Alias::NeedsAliasPreflight(wrong_base_uint, sampled, false));
        assert(Bc4Alias::RejectLayout(sampled, wrong_base_uint));
        assert(Bc4Alias::RejectLayout(wrong_base_uint, sampled));
        auto wrong_shape_uint = wrong_base_uint;
        wrong_shape_uint.guest_address = sampled.guest_address;
        assert(Bc4Alias::NeedsAliasPreflight(sampled, wrong_shape_uint, false));
        assert(Bc4Alias::NeedsAliasPreflight(wrong_shape_uint, sampled, false));
        assert(Bc4Alias::RejectLayout(sampled, wrong_shape_uint));
        assert(Bc4Alias::RejectLayout(wrong_shape_uint, sampled));
    }
    {
        ImageInfo tail{};
        tail.pixel_format = vk::Format::eR32G32Sfloat;
        tail.type = AmdGpu::ImageType::Color2D;
        tail.size = {1, 1, 1};
        tail.resources = {1, 1};
        tail.num_samples = 1;
        tail.num_bits = 64;
        tail.pitch = 8;
        tail.guest_address = 0x105c00;
        tail.guest_size = 512;
        tail.props.is_tiled = true;
        tail.tile_mode = AmdGpu::TileMode::Thin1DThin;
        tail.array_mode = AmdGpu::ArrayMode::Array1DTiledThin1;
        tail.mips_layout[0] = {512, 8, 8, 0};
        assert(Bc4Alias::IsFloatTailCandidate(tail));
        const auto reject = [&](auto mutate) {
            auto invalid = tail;
            mutate(invalid);
            assert(!Bc4Alias::IsFloatTailCandidate(invalid));
        };
        reject([](auto& i) { i.pixel_format = vk::Format::eR32G32Uint; });
        reject([](auto& i) { i.size.width = 2; });
        reject([](auto& i) { i.size.height = 2; });
        reject([](auto& i) { i.resources.levels = 2; });
        reject([](auto& i) { i.resources.layers = 2; });
        reject([](auto& i) { i.num_samples = 2; });
        reject([](auto& i) { i.num_bits = 32; });
        reject([](auto& i) { i.pitch = 1; });
        reject([](auto& i) { i.guest_size = 8; });
        reject([](auto& i) { i.mips_layout[0].offset = 8; });
        reject([](auto& i) { i.mips_layout[0].size = 8; });
        reject([](auto& i) { i.mips_layout[0].pitch = 1; });
        reject([](auto& i) { i.mips_layout[0].height = 1; });
        reject([](auto& i) { i.props.is_block = true; });
        reject([](auto& i) { i.props.is_tiled = false; });
        reject([](auto& i) { i.meta_info.htile_addr = 0x200000; });
        reject([](auto& i) { i.bank_swizzle = 1; });
        reject([](auto& i) { i.alt_tile = true; });
#ifndef BC4_ALIAS_POLICY_ONLY
        // Real MipOf/SliceOf run in the linked CPU mode, with independent fixed mip layout.
        auto parent = bc;
        parent.props.is_tiled = true;
        parent.tile_mode = tail.tile_mode;
        parent.array_mode = tail.array_mode;
        parent.size = {256, 128, 1};
        parent.pitch = 256;
        parent.resources.levels = 9;
        parent.guest_size = 0x6000;
        const u32 offsets[]{0, 0x4000, 0x5000, 0x5400, 0x5600, 0x5800, 0x5a00, 0x5c00, 0x5e00};
        for (u32 mip = 0; mip < 9; ++mip) {
            const u32 pitch = std::max(256u >> mip, 32u);
            const u32 height = std::max(128u >> mip, 32u);
            parent.mips_layout[mip] = {pitch * height / 2, pitch, height, offsets[mip]};
        }
        assert(tail.MipOf(parent) == 7 && tail.SliceOf(parent, 7) == 0);
        assert(!Bc4Alias::RejectFloatTail(tail, parent, 7));
        assert(Bc4Alias::RejectFloatTail(tail, parent, 6));
        assert(Bc4Alias::RejectFloatTail(tail, parent, 8));
        auto wrong_address = tail;
        wrong_address.guest_address += 8;
        assert(Bc4Alias::RejectFloatTail(wrong_address, parent, 7));
        auto wrong_parent = parent;
        wrong_parent.resources.layers = 2;
        assert(Bc4Alias::RejectFloatTail(tail, wrong_parent, 7));
        wrong_parent = parent; wrong_parent.mips_layout[7].size = 1024;
        assert(Bc4Alias::RejectFloatTail(tail, wrong_parent, 7));
        wrong_parent = parent; wrong_parent.guest_size = offsets[7] + 8;
        assert(Bc4Alias::RejectFloatTail(tail, wrong_parent, 7));
        // A prospective raw writer on another mip must be compared with the whole parent.
        assert(!Bc4Alias::RangesOverlap(tail.guest_address, 512, parent.guest_address + 8, 8));
        assert(Bc4Alias::RangesOverlap(parent.guest_address, parent.guest_size, parent.guest_address + 8, 8));
#endif
    }
    for (bool tiled : {false, true}) {
        auto a = bc, b = ui;
        if (tiled) {
            a.props.is_tiled = b.props.is_tiled = true;
            a.tile_mode = b.tile_mode = AmdGpu::TileMode::Thin1DThin;
            a.array_mode = b.array_mode = AmdGpu::ArrayMode::Array1DTiledThin1;
        }
        assert(!Bc4Alias::RejectLayout(a, b));
        assert(!Bc4Alias::RejectStoredLayout(a));
        assert(!Bc4Alias::RejectStoredLayout(b));
        const auto reject = [&](auto mutate) {
            auto bad = b;
            mutate(bad);
            assert(Bc4Alias::RejectLayout(a, bad));
            assert(Bc4Alias::RejectLayout(bad, a));
        };
        reject([](auto& i) { ++i.guest_address; });
        reject([](auto& i) { ++i.guest_size; });
        reject([](auto& i) { i.resources.levels = 2; });
        reject([](auto& i) { i.resources.layers = 2; });
        reject([](auto& i) { i.type = AmdGpu::ImageType::Color3D; });
        reject([](auto& i) { i.num_samples = 2; });
        reject([](auto& i) { i.mips_layout[0].offset = 8; });
        reject([](auto& i) { ++i.mips_layout[0].size; });
        reject([](auto& i) { ++i.mips_layout[0].pitch; });
        reject([](auto& i) { ++i.mips_layout[0].height; });
        reject([](auto& i) { ++i.pitch; });
        reject([](auto& i) { ++i.size.width; });
        reject([](auto& i) { i.meta_info.cmask_addr = 0x200000; });
        reject([](auto& i) { i.bank_swizzle = 1; });
        reject([](auto& i) { i.alt_tile = true; });
        reject([](auto& i) { i.props.is_pow2 = !i.props.is_pow2; });
        reject([](auto& i) { i.pixel_format = vk::Format::eR32G32Sfloat; });
    }
    assert(Bc4Alias::CompleteSubresource(0, 0, 1, 1));
    assert(!Bc4Alias::CompleteSubresource(1, 0, 1, 1));
    assert(!Bc4Alias::CompleteSubresource(0, 1, 1, 1));
    assert(!Bc4Alias::CompleteSubresource(0, 0, 2, 1));
    assert(!Bc4Alias::CompleteSubresource(0, 0, 1, 2));
}

#ifdef BC4_ALIAS_POLICY_ONLY
int main() { RunBc4AliasPolicy(); }
#endif
