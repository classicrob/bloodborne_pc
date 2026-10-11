// SPDX-License-Identifier: GPL-2.0-or-later
// The replacement half of the RT fill differential (vk_shader_hle.cpp FillAsImageClear) on the
// device: for every supported format and value, vkCmdClearColorImage must leave every texel equal
// to FillClear::Texel (the bytes the SDK fill kernel's typed store writes), and vkCmdFillBuffer
// with FillClear::Pattern must leave every word of a padded range equal to it with the bytes
// after it untouched. The kernel half (the recompiled kernel against the ISA reference) is in the
// GCN conformance suite.
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <optional>
#include "video_core/renderer_vulkan/fill_clear.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/image.h"
#include <vk_mem_alloc.h>

using namespace Vulkan::FillClear;

namespace {
unsigned failures = 0;
void Expect(const std::string& what, bool ok) {
    std::printf("%-78s %s\n", what.c_str(), ok ? "ok" : "FAIL");
    failures += ok ? 0 : 1;
}

vk::Format ImageFormat(Format format) {
    switch (format) {
    case Format::Rgba16Float:
        return vk::Format::eR16G16B16A16Sfloat;
    case Format::B10G11R11Ufloat:
        return vk::Format::eB10G11R11UfloatPack32;
    case Format::Rgba8Unorm:
        return vk::Format::eR8G8B8A8Unorm;
    }
    return vk::Format::eUndefined;
}

const char* Name(Format format) {
    return format == Format::Rgba16Float      ? "RGBA16F"
           : format == Format::B10G11R11Ufloat ? "B10G11R11"
                                               : "RGBA8 unorm";
}
} // namespace

int main() {
    Vulkan::Instance instance(0, false);
    Vulkan::Scheduler scheduler(instance);
    const auto device = instance.GetDevice();
    // An odd size, so the image is not a power of two and the buffer range is padded past it.
    constexpr u32 W = 37, H = 11, Texels = W * H, PaddingWords = 37, CanaryWords = 16;

    // Host buffer: [image readback | fill range (texels + padding) | canary].
    constexpr u64 MaxTexel = 8;
    constexpr u64 Bytes = Texels * MaxTexel * 2 + (PaddingWords + CanaryWords) * 4;
    VkBuffer host{};
    VmaAllocation allocation{};
    VmaAllocationInfo ai{};
    const VkBufferCreateInfo bi{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                .size = Bytes,
                                .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                         VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    const VmaAllocationCreateInfo ac{.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                                              VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
                                     .usage = VMA_MEMORY_USAGE_AUTO,
                                     .requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT};
    assert(vmaCreateBuffer(instance.GetAllocator(), &bi, &ac, &host, &allocation, &ai) ==
           VK_SUCCESS);
    auto* bytes = static_cast<u8*>(ai.pMappedData);

    struct Case {
        Format format;
        std::array<float, 4> value;
        const char* label;
    };
    const std::array<Case, 11> cases = {{
        {Format::Rgba16Float, {0, 0, 0, 0}, "zero"},
        {Format::Rgba16Float, {1, 1, 1, 1}, "ones"},
        {Format::Rgba16Float, {1, 0.5f, 0.25f, 2}, "unequal channels"},
        {Format::Rgba16Float, {-2, 0.75f, -2, 0.75f}, "unequal, halves equal (pattern)"},
        {Format::B10G11R11Ufloat, {0, 0, 0, 0}, "zero"},
        {Format::B10G11R11Ufloat, {1, 1, 1, 1}, "ones"},
        {Format::B10G11R11Ufloat, {1, 0, 2, 0.25f}, "unequal channels"},
        {Format::B10G11R11Ufloat, {0.5f, 3, 0.125f, 0}, "unequal channels 2"},
        {Format::Rgba8Unorm, {0, 0, 0, 0}, "zero"},
        {Format::Rgba8Unorm, {1, 1, 1, 1}, "ones"},
        {Format::Rgba8Unorm, {1, 0, 1, 1}, "unequal channels"},
    }};

    for (const Case& c : cases) {
        const std::string label = std::string{Name(c.format)} + ", " + c.label;
        const auto texel = Texel(c.format, c.value);
        if (!texel) {
            Expect(label + ": value accepted by the gate", false);
            continue;
        }
        const u32 texel_bytes = TexelBytes(c.format);
        const auto pattern = Pattern(c.format, *texel);
        std::memset(bytes, 0xA5, Bytes);

        VideoCore::UniqueImage image(device, instance.GetAllocator());
        image.Create({.imageType = vk::ImageType::e2D,
                      .format = ImageFormat(c.format),
                      .extent = {W, H, 1},
                      .mipLevels = 1,
                      .arrayLayers = 1,
                      .samples = vk::SampleCountFlagBits::e1,
                      .tiling = vk::ImageTiling::eOptimal,
                      .usage = vk::ImageUsageFlagBits::eTransferSrc |
                               vk::ImageUsageFlagBits::eTransferDst});
        const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
        const auto cmd = scheduler.CommandBuffer();
        const auto transition = [&](vk::ImageLayout from, vk::ImageLayout to) {
            const vk::ImageMemoryBarrier2 barrier{
                .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
                .oldLayout = from,
                .newLayout = to,
                .image = vk::Image(image),
                .subresourceRange = range};
            cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
        };
        transition(vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal);
        vk::ClearColorValue clear{};
        clear.float32 = c.value;
        cmd.clearColorImage(vk::Image(image), vk::ImageLayout::eTransferDstOptimal, clear, range);
        transition(vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eTransferSrcOptimal);
        const vk::BufferImageCopy region{.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                                         .imageExtent = {W, H, 1}};
        cmd.copyImageToBuffer(vk::Image(image), vk::ImageLayout::eTransferSrcOptimal, host, region);
        // The bytes the kernel writes: the pattern over texels plus padding, then canaries.
        const u64 fill_offset = Texels * MaxTexel;
        const u64 fill_bytes = Texels * u64(texel_bytes) + PaddingWords * 4;
        if (pattern) {
            cmd.fillBuffer(host, fill_offset, fill_bytes, *pattern);
        }
        const vk::MemoryBarrier2 to_host{.srcStageMask = vk::PipelineStageFlagBits2::eAllTransfer,
                                         .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                                         .dstStageMask = vk::PipelineStageFlagBits2::eHost,
                                         .dstAccessMask = vk::AccessFlagBits2::eHostRead};
        cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &to_host});
        scheduler.Finish();

        u32 texel_mismatches = 0;
        for (u32 t = 0; t < Texels; ++t) {
            texel_mismatches +=
                std::memcmp(bytes + t * texel_bytes, texel->data(), texel_bytes) != 0;
        }
        char detail[160];
        std::snprintf(detail, sizeof(detail), "%s: clear: %u of %u texels differ from the typed-store bytes",
                      label.c_str(), texel_mismatches, Texels);
        Expect(detail, texel_mismatches == 0);
        if (!pattern) {
            Expect(label + ": no single fill word (halves differ): the kernel runs", texel_bytes == 8);
            continue;
        }
        u32 word_mismatches = 0;
        for (u64 at = 0; at < fill_bytes; at += 4) {
            u32 word;
            std::memcpy(&word, bytes + fill_offset + at, 4);
            word_mismatches += word != *pattern;
        }
        u32 canaries = 0;
        for (u64 at = fill_offset + fill_bytes; at < fill_offset + fill_bytes + CanaryWords * 4;
             ++at) {
            canaries += bytes[at] != 0xA5;
        }
        // Every element the kernel stores is the texel: the fill must equal the texel repeated.
        u32 element_mismatches = 0;
        for (u64 at = 0; at + texel_bytes <= Texels * u64(texel_bytes); at += texel_bytes) {
            element_mismatches += std::memcmp(bytes + fill_offset + at, texel->data(), texel_bytes) != 0;
        }
        std::snprintf(detail, sizeof(detail),
                      "%s: fill %#010x: %u words, %u elements wrong; %u canary bytes changed",
                      label.c_str(), *pattern, word_mismatches, element_mismatches, canaries);
        Expect(detail, word_mismatches == 0 && element_mismatches == 0 && canaries == 0);
    }
    vmaDestroyBuffer(instance.GetAllocator(), host, allocation);
    std::printf("fill-clear-device-test: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
