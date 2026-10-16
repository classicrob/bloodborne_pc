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
