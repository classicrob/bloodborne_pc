// SPDX-License-Identifier: GPL-2.0-or-later
// The perf replay's image records (video_core/texture_cache/capture_images.h): the state blob
// round-trips and a blob from another layout or version is refused, the per-aspect texel sizes
// match Vulkan's packed buffer layouts (and unsupported formats are refused, not guessed), and
// the record sizes follow the mip chain.
#include <cassert>
#include <cstdio>
#include <string>
#include "video_core/texture_cache/capture_images.h"

using namespace VideoCore;

namespace {
unsigned failures = 0;
void Expect(const char* what, bool ok) {
    std::printf("%-78s %s\n", what, ok ? "ok" : "FAIL");
    failures += ok ? 0 : 1;
}
} // namespace

int main() {
    CapturedImageState state;
    state.flags = 0x8;
    state.usage = 0b00101;
    state.aspect_mask = static_cast<u32>(vk::ImageAspectFlagBits::eColor);
    state.num_mip_hashes = 2;
    state.mip_hashes[0] = 0x1234;
    state.mip_hashes[1] = 0x5678;
    state.tracked = 1;
    state.image_uid = 77;
    state.depth_uid = 78;
    state.hash = 0xabcdef;
    state.info.guest_address = 0x7040000000;
    state.info.guest_size = 1920 * 1080 * 4;
    state.info.size = {1920, 1080, 1};
    state.info.pixel_format = vk::Format::eR8G8B8A8Unorm;
    state.info.resources.levels = 1;
    state.info.resources.layers = 1;
    const auto bytes = EncodeState(state);
    std::string why;
    const auto decoded = DecodeState(bytes, why);
    Expect("state blob round-trips byte for byte",
           decoded && std::memcmp(&*decoded, &state, sizeof(state)) == 0);

    auto short_blob = bytes;
    short_blob.pop_back();
    Expect("a blob of another size is refused", !DecodeState(short_blob, why) && !why.empty());
    auto other_version = bytes;
    other_version[4] ^= 0xff; // version
    why.clear();
    Expect("a blob of another version is refused", !DecodeState(other_version, why) && !why.empty());
    auto other_layout = bytes;
    other_layout[8] ^= 0x01; // info_bytes
    why.clear();
    Expect("a blob of another ImageInfo layout is refused",
           !DecodeState(other_layout, why) && !why.empty());
    auto too_many_hashes = state;
    too_many_hashes.num_mip_hashes = 17;
    Expect("more than 16 mip hashes are refused",
           !DecodeState(EncodeState(too_many_hashes), why));

    using A = vk::ImageAspectFlagBits;
    Expect("RGBA8 color: 4 bytes", AspectTexelBytes(vk::Format::eR8G8B8A8Unorm, A::eColor) == 4);
    Expect("RGBA16F color: 8 bytes",
           AspectTexelBytes(vk::Format::eR16G16B16A16Sfloat, A::eColor) == 8);
    Expect("B10G11R11 color: 4 bytes",
           AspectTexelBytes(vk::Format::eB10G11R11UfloatPack32, A::eColor) == 4);
    Expect("R32F color: 4 bytes", AspectTexelBytes(vk::Format::eR32Sfloat, A::eColor) == 4);
    Expect("BC1 (block-compressed) refused", AspectTexelBytes(vk::Format::eBc1RgbaUnormBlock,
                                                              A::eColor) == 0);
    Expect("D32S8: depth 4, stencil 1",
           AspectTexelBytes(vk::Format::eD32SfloatS8Uint, A::eDepth) == 4 &&
               AspectTexelBytes(vk::Format::eD32SfloatS8Uint, A::eStencil) == 1);
    Expect("D16: depth 2, no stencil", AspectTexelBytes(vk::Format::eD16Unorm, A::eDepth) == 2 &&
                                           AspectTexelBytes(vk::Format::eD16Unorm,
                                                            A::eStencil) == 0);
    Expect("D32: depth 4", AspectTexelBytes(vk::Format::eD32Sfloat, A::eDepth) == 4);
    Expect("a color format asked for depth is refused",
           AspectTexelBytes(vk::Format::eR8G8B8A8Unorm, A::eDepth) == 0);

    const auto split = SplitAspects(A::eDepth | A::eStencil);
    Expect("depth+stencil splits into depth then stencil",
           split.size() == 2 && split[0] == A::eDepth && split[1] == A::eStencil);

    ImageInfo info;
    info.size = {37, 11, 1};
    info.resources.levels = 3;
    info.resources.layers = 6;
    Expect("mip 2 of 37x11 is 9x2", MipExtent(info, 2).width == 9 && MipExtent(info, 2).height == 2);
    Expect("mip extents never reach 0", MipExtent(info, 9).width == 1 &&
                                            MipExtent(info, 9).height == 1);
    Expect("aspect bytes count every layer", AspectBytes(info, 0, 4) == 37ull * 11 * 4 * 6);
    info.size = {65536, 65536, 1};
    info.resources.layers = 2;
    Expect("an aspect over 4 GiB is refused (0)", AspectBytes(info, 0, 4) == 0);

    std::printf("capture-images-test: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
