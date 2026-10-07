// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstring>
#include <map>
#include <tuple>
#include "common/assert.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

// Descriptor placeholders stay alive for the rasterizer lifetime. Shader guards supply
// null semantics, including zero query results and discarded stores/atomics.
class NullResources {
public:
    NullResources(vk::PhysicalDevice physical_, vk::Device device_)
        : physical{physical_}, device{device_} {
        const vk::BufferCreateInfo info{
            .size = BufferSize,
            .usage = vk::BufferUsageFlagBits::eStorageBuffer |
                     vk::BufferUsageFlagBits::eVertexBuffer,
        };
        const auto [result, handle] = device.createBuffer(info);
        ASSERT_MSG(result == vk::Result::eSuccess, "macOS null buffer creation failed: {}",
                   vk::to_string(result));
        buffer = handle;
        const auto requirements = device.getBufferMemoryRequirements(buffer);
        buffer_memory = Allocate(requirements, vk::MemoryPropertyFlagBits::eHostVisible |
                                                   vk::MemoryPropertyFlagBits::eHostCoherent);
        ASSERT_MSG(device.bindBufferMemory(buffer, buffer_memory, 0) == vk::Result::eSuccess,
                   "macOS null buffer binding failed");
        const auto [map_result, data] = device.mapMemory(buffer_memory, 0, BufferSize);
        ASSERT_MSG(map_result == vk::Result::eSuccess, "macOS null buffer mapping failed");
        std::memset(data, 0, BufferSize);
        device.unmapMemory(buffer_memory);
    }

    ~NullResources() {
        for (const auto& [key, entry] : images) {
            device.destroyImageView(entry.view);
            device.destroyImage(entry.image);
            device.freeMemory(entry.memory);
        }
        device.destroyBuffer(buffer);
        device.freeMemory(buffer_memory);
    }

    NullResources(const NullResources&) = delete;
    NullResources& operator=(const NullResources&) = delete;

    vk::Buffer Buffer() const { return buffer; }
    static constexpr u64 BufferSize = 64;

    vk::ImageView ImageView(vk::ImageViewType type, vk::Format format,
                            vk::SampleCountFlagBits samples, bool storage,
                            auto&& initialize) {
        const auto key = std::make_tuple(type, format, samples, storage);
        if (const auto it = images.find(key); it != images.end()) {
            return it->second.view;
        }
        const bool depth = format == vk::Format::eD32Sfloat;
        const auto image_type = type == vk::ImageViewType::e1D || type == vk::ImageViewType::e1DArray
                                    ? vk::ImageType::e1D
                                    : type == vk::ImageViewType::e3D ? vk::ImageType::e3D
                                                                   : vk::ImageType::e2D;
        const vk::ImageCreateInfo info{
            .imageType = image_type,
            .format = format,
            .extent = {1, 1, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = samples,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc |
                     (storage ? vk::ImageUsageFlagBits::eStorage : vk::ImageUsageFlagBits::eSampled),
        };
        Entry entry;
        const auto [result, image] = device.createImage(info);
        ASSERT_MSG(result == vk::Result::eSuccess,
                   "macOS typed null image creation failed ({}, {}, {}): {}", vk::to_string(type),
                   vk::to_string(format), vk::to_string(samples), vk::to_string(result));
        entry.image = image;
        entry.memory = Allocate(device.getImageMemoryRequirements(image), {});
        ASSERT_MSG(device.bindImageMemory(image, entry.memory, 0) == vk::Result::eSuccess,
                   "macOS typed null image binding failed");
        const vk::ImageSubresourceRange range{
            .aspectMask = depth ? vk::ImageAspectFlagBits::eDepth : vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1,
        };
        const vk::ImageViewCreateInfo view_info{
            .image = image, .viewType = type, .format = format, .subresourceRange = range,
        };
        const auto [view_result, view] = device.createImageView(view_info);
        ASSERT_MSG(view_result == vk::Result::eSuccess, "macOS typed null image view failed: {}",
                   vk::to_string(view_result));
        entry.view = view;
        initialize([image, range, depth](vk::CommandBuffer command) {
            vk::ImageMemoryBarrier barrier{
                .srcAccessMask = {}, .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
                .oldLayout = vk::ImageLayout::eUndefined, .newLayout = vk::ImageLayout::eGeneral,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = image, .subresourceRange = range,
            };
            command.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
                                    vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, barrier);
            if (depth) {
                command.clearDepthStencilImage(image, vk::ImageLayout::eGeneral,
                                               vk::ClearDepthStencilValue{.depth = 0.0f, .stencil = 0}, range);
            } else {
                command.clearColorImage(image, vk::ImageLayout::eGeneral,
                                        vk::ClearColorValue{}, range);
            }
            barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
            barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead;
            barrier.oldLayout = vk::ImageLayout::eGeneral;
            command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                                    vk::PipelineStageFlagBits::eAllCommands, {}, {}, {}, barrier);
        });
        images.emplace(key, entry);
        return view;
    }

private:
    vk::DeviceMemory Allocate(const vk::MemoryRequirements& requirements,
                              vk::MemoryPropertyFlags wanted) {
        const auto memory = physical.getMemoryProperties();
        for (u32 i = 0; i < memory.memoryTypeCount; ++i) {
            if ((requirements.memoryTypeBits & (1u << i)) &&
                (memory.memoryTypes[i].propertyFlags & wanted) == wanted) {
                const auto [result, allocation] = device.allocateMemory(vk::MemoryAllocateInfo{.allocationSize = requirements.size, .memoryTypeIndex = i});
                ASSERT_MSG(result == vk::Result::eSuccess, "macOS null resource allocation failed");
                return allocation;
            }
        }
        UNREACHABLE_MSG("macOS null resources require a compatible Vulkan memory type");
    }

    struct Entry { vk::Image image; vk::ImageView view; vk::DeviceMemory memory; };
    vk::PhysicalDevice physical;
    vk::Device device;
    vk::Buffer buffer;
    vk::DeviceMemory buffer_memory;
    std::map<std::tuple<vk::ImageViewType, vk::Format, vk::SampleCountFlagBits, bool>, Entry> images;
};

} // namespace Vulkan
