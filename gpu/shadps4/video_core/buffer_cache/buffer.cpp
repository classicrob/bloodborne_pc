// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <numeric>
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif

#include "common/alignment.h"
#include "common/assert.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

#include <vk_mem_alloc.h>

namespace VideoCore {

std::string_view BufferTypeName(MemoryType type) {
    switch (type) {
    case MemoryType::HostUncached:
        return "HostUncached";
    case MemoryType::HostCached:
        return "HostCached";
    case MemoryType::Stream:
        return "Stream";
    case MemoryType::DeviceLocal:
        return "DeviceLocal";
    case MemoryType::Sparse:
        return "Sparse";
#ifdef __APPLE__
    case MemoryType::ImportedHost:
        return "ImportedHost";
#endif
    default:
        return "Invalid";
    }
}

[[nodiscard]] VkMemoryPropertyFlags MemoryUsagePreferredVmaFlags(MemoryType type) {
    return type != MemoryType::DeviceLocal ? VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                           : VkMemoryPropertyFlagBits{};
}

[[nodiscard]] VmaAllocationCreateFlags MemoryUsageVmaFlags(MemoryType type) {
    switch (type) {
    case MemoryType::HostUncached:
    case MemoryType::Stream:
        return VMA_ALLOCATION_CREATE_MAPPED_BIT |
               VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    case MemoryType::HostCached:
        return VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
    case MemoryType::DeviceLocal:
    default:
        return {};
    }
}

[[nodiscard]] VmaMemoryUsage MemoryUsageVma(MemoryType type) {
    switch (type) {
    case MemoryType::DeviceLocal:
    case MemoryType::Stream:
        return VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    case MemoryType::HostUncached:
    case MemoryType::HostCached:
        return VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    default:
        return VMA_MEMORY_USAGE_UNKNOWN;
    }
}

UniqueBuffer::UniqueBuffer(vk::Device device_, VmaAllocator allocator_)
    : device{device_}, allocator{allocator_} {}

UniqueBuffer::~UniqueBuffer() {
    Destroy();
}

void UniqueBuffer::Destroy() {
    if (allocation) {
        vmaDestroyBuffer(allocator, buffer, allocation);
    } else if (buffer) {
        device.destroyBuffer(buffer);
    }
#ifdef __APPLE__
    if (imported_memory) {
        device.freeMemory(imported_memory);
        imported_memory = VK_NULL_HANDLE;
    }
    if (imported_host_alias) {
        const auto result = mach_vm_deallocate(mach_task_self(), imported_host_alias, imported_host_size);
        ASSERT_MSG(result == KERN_SUCCESS, "macOS GPU import alias retirement failed: {}", result);
        imported_host_alias = 0;
        imported_host_size = 0;
    }
#endif
    buffer = VK_NULL_HANDLE;
    allocation = VK_NULL_HANDLE;
    bda_addr = 0;
}

void UniqueBuffer::Create(vk::BufferCreateInfo& buffer_ci, MemoryType mem_type,
                          VmaAllocationInfo* out_alloc_info) {
#ifdef __APPLE__
    ASSERT_MSG(mem_type != MemoryType::Sparse && mem_type != MemoryType::ImportedHost,
               "macOS guest buffers require the non-sparse host import path");
#endif
    const bool with_bda = bool(buffer_ci.usage & vk::BufferUsageFlagBits::eShaderDeviceAddress);
    if (mem_type != MemoryType::Sparse) {
        const VmaAllocationCreateFlags bda_flag =
            with_bda ? VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT : 0;
        const VmaAllocationCreateInfo alloc_ci = {
            .flags =
                VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT | bda_flag | MemoryUsageVmaFlags(mem_type),
            .usage = MemoryUsageVma(mem_type),
            .requiredFlags = 0,
            .preferredFlags = MemoryUsagePreferredVmaFlags(mem_type),
            .pool = VK_NULL_HANDLE,
            .pUserData = nullptr,
        };

        const VkBufferCreateInfo buffer_ci_unsafe = static_cast<VkBufferCreateInfo>(buffer_ci);
        VkBuffer unsafe_buffer{};
        VkResult result = vmaCreateBuffer(allocator, &buffer_ci_unsafe, &alloc_ci, &unsafe_buffer,
                                          &allocation, out_alloc_info);
        ASSERT_MSG(result == VK_SUCCESS, "Failed allocating buffer with error {}",
                   vk::to_string(vk::Result{result}));
        buffer = vk::Buffer{unsafe_buffer};
    } else {
        buffer_ci.flags |=
            vk::BufferCreateFlagBits::eSparseBinding | vk::BufferCreateFlagBits::eSparseResidency;
        buffer = Vulkan::Check(device.createBuffer(buffer_ci));
    }

    if (with_bda) {
        const vk::BufferDeviceAddressInfo bda_info = {
            .buffer = buffer,
        };
        auto bda_result = device.getBufferAddress(bda_info);
        ASSERT_MSG(bda_result != 0, "Failed to get buffer device address");
        bda_addr = bda_result;
    }
}

#ifdef __APPLE__
void UniqueBuffer::ImportHost(vk::PhysicalDevice physical, vk::BufferCreateInfo info, void* pointer) {
    const auto props = physical.getProperties2<vk::PhysicalDeviceProperties2,
        vk::PhysicalDeviceExternalMemoryHostPropertiesEXT>();
    const u64 alignment = props.get<vk::PhysicalDeviceExternalMemoryHostPropertiesEXT>()
                              .minImportedHostPointerAlignment;
    ASSERT_MSG(info.size && info.size % alignment == 0 &&
                   reinterpret_cast<uintptr_t>(pointer) % alignment == 0,
               "macOS guest host import {:#x}+{:#x} violates alignment {}",
               reinterpret_cast<uintptr_t>(pointer), info.size, alignment);
    mach_vm_address_t alias = 0;
    vm_prot_t current = 0, maximum = 0;
    auto result = mach_vm_remap(mach_task_self(), &alias, info.size, alignment - 1, VM_FLAGS_ANYWHERE,
        mach_task_self(), reinterpret_cast<uintptr_t>(pointer), FALSE, &current, &maximum, VM_INHERIT_NONE);
    ASSERT_MSG(result == KERN_SUCCESS,
               "macOS GPU host import alias for {:#x}+{:#x} failed: {}",
               reinterpret_cast<uintptr_t>(pointer), info.size, result);
    imported_host_alias = alias;
    imported_host_size = info.size;
    result = mach_vm_protect(mach_task_self(), alias, info.size, FALSE, VM_PROT_READ | VM_PROT_WRITE);
    ASSERT_MSG(result == KERN_SUCCESS,
               "macOS GPU host import requires a writable backing alias: guest={:#x}, max_prot={}, error={}",
               reinterpret_cast<uintptr_t>(pointer), maximum, result);
    pointer = reinterpret_cast<void*>(alias);
    const vk::ExternalMemoryBufferCreateInfo external{
        .handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT,
    };
    info.pNext = &external;
    info.flags = {};
    buffer = Vulkan::Check(device.createBuffer(info));
    const auto requirements = device.getBufferMemoryRequirements(buffer);
    ASSERT_MSG(requirements.size <= info.size,
               "macOS host import memory requirement {:#x} exceeds live guest span {:#x}",
               requirements.size, info.size);
    const auto host = Vulkan::Check(device.getMemoryHostPointerPropertiesEXT(
        vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT, pointer));
    const auto memory = physical.getMemoryProperties();
    const auto wanted = vk::MemoryPropertyFlagBits::eDeviceLocal |
                        vk::MemoryPropertyFlagBits::eHostVisible |
                        vk::MemoryPropertyFlagBits::eHostCoherent;
    u32 type = memory.memoryTypeCount;
    for (u32 index = 0; index < memory.memoryTypeCount; ++index) {
        if ((requirements.memoryTypeBits & host.memoryTypeBits & (1u << index)) &&
            (memory.memoryTypes[index].propertyFlags & wanted) == wanted) {
            type = index;
            break;
        }
    }
    ASSERT_MSG(type < memory.memoryTypeCount,
               "macOS guest host import requires coherent device-local host-visible memory");
    const vk::MemoryAllocateFlagsInfo flags{
        .flags = vk::MemoryAllocateFlagBits::eDeviceAddress,
    };
    const vk::ImportMemoryHostPointerInfoEXT import{
        .pNext = &flags, .handleType = vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT,
        .pHostPointer = pointer,
    };
    imported_memory = Vulkan::Check(device.allocateMemory(vk::MemoryAllocateInfo{
        .pNext = &import, .allocationSize = requirements.size, .memoryTypeIndex = type,
    }));
    Vulkan::Check(device.bindBufferMemory(buffer, imported_memory, 0));
    bda_addr = device.getBufferAddress(vk::BufferDeviceAddressInfo{.buffer = buffer});
    ASSERT_MSG(bda_addr, "macOS guest host import {:#x}+{:#x} has no buffer device address",
               reinterpret_cast<uintptr_t>(pointer), info.size);
}
#endif

Buffer::Buffer(const Vulkan::Instance& instance, VAddr cpu_addr_, u64 size_bytes_,
               MemoryType mem_type_, std::string_view debug_name)
    : cpu_addr{cpu_addr_}, size_bytes{size_bytes_}, mem_type{mem_type_},
      buffer{instance.GetDevice(), instance.GetAllocator()} {

    vk::BufferCreateInfo buffer_ci = {
        .size = size_bytes,
        .usage = AllFlags,
        .sharingMode = vk::SharingMode::eExclusive,
    };
    VmaAllocationInfo alloc_info{};
#ifdef __APPLE__
    if (mem_type == MemoryType::ImportedHost) {
        buffer.ImportHost(instance.GetPhysicalDevice(), buffer_ci,
                          reinterpret_cast<void*>(cpu_addr));
        mapped_data = {reinterpret_cast<u8*>(cpu_addr), size_bytes};
        is_coherent = true;
    } else
#endif
    {
        buffer.Create(buffer_ci, mem_type, &alloc_info);
    }

    const auto device = instance.GetDevice();
    if (!debug_name.empty()) {
        Vulkan::SetObjectName(device, Handle(), debug_name);
    } else {
        Vulkan::SetObjectName(device, Handle(), "Buffer {:#x}:{:#x}", cpu_addr, size_bytes);
    }

    if (mem_type != MemoryType::Sparse
#ifdef __APPLE__
        && mem_type != MemoryType::ImportedHost
#endif
    ) {
        VkMemoryPropertyFlags property_flags{};
        vmaGetAllocationMemoryProperties(instance.GetAllocator(), buffer.allocation,
                                         &property_flags);
        if (alloc_info.pMappedData) {
            mapped_data = std::span<u8>{std::bit_cast<u8*>(alloc_info.pMappedData), size_bytes};
        }
        is_coherent = property_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    }
}

void Buffer::Flush(u64 offset, u64 size) {
    if (mapped_data.empty() || is_coherent) {
        return;
    }
    vmaFlushAllocation(buffer.allocator, buffer.allocation, offset, size);
}

void Buffer::Invalidate(u64 offset, u64 size) {
    if (mapped_data.empty() || is_coherent) {
        return;
    }
    vmaInvalidateAllocation(buffer.allocator, buffer.allocation, offset, size);
}

constexpr u64 WATCHES_INITIAL_RESERVE = 0x100;
constexpr u64 WATCHES_RESERVE_CHUNK = 0x100;

StreamBuffer::StreamBuffer(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler_,
                           MemoryType mem_type, u64 size_bytes)
    : Buffer{instance, 0, size_bytes, mem_type}, scheduler{scheduler_},
      non_coherent_atom_size{instance.NonCoherentAtomSize()} {
    ReserveWatches(current_watches, WATCHES_INITIAL_RESERVE);
    ReserveWatches(previous_watches, WATCHES_INITIAL_RESERVE);
    Vulkan::SetObjectName(instance.GetDevice(), Handle(), "StreamBuffer({}):{:#x}",
                          BufferTypeName(mem_type), size_bytes);
}

bool StreamBuffer::PrepareMap(u64 size, u64 alignment, bool allow_wait) {
    if (!mapped_data.empty() && !is_coherent) {
        size = Common::AlignUp(size, non_coherent_atom_size);
        alignment =
            alignment > 0 ? std::lcm(alignment, non_coherent_atom_size) : non_coherent_atom_size;
    }

    if (size > this->size_bytes) {
        return false;
    }

    if (alignment > 0) {
        offset = Common::AlignUp(offset, alignment);
    }

    if (offset + size > this->size_bytes) {
        // The buffer would overflow, save the amount of used watches and reset the state.
        invalidation_mark = current_watch_cursor;
        current_watch_cursor = 0;
        offset = 0;

        // Swap watches and reset waiting cursors.
        std::swap(previous_watches, current_watches);
        wait_cursor = 0;
        wait_bound = 0;
    }

    if (!WaitPendingOperations(offset + size, allow_wait)) {
        return false;
    }

    mapped_size = size;
    return true;
}

std::pair<u8*, u64> StreamBuffer::Map(u64 size, u64 alignment, bool allow_wait) {
    if (!PrepareMap(size, alignment, allow_wait)) {
        return {nullptr, 0};
    }
    u8* const data = mapped_data.empty() ? nullptr : mapped_data.data() + offset;
    return {data, offset};
}

void StreamBuffer::Commit() {
    if (mem_type == MemoryType::HostCached) {
        Invalidate(offset, mapped_size);
    } else {
        Flush(offset, mapped_size);
    }
    AdvanceAndWatch();
}

std::optional<u64> StreamBuffer::Reserve(u64 size, u64 alignment, bool allow_wait) {
    if (!PrepareMap(size, alignment, allow_wait)) {
        return std::nullopt;
    }
    const u64 reserved_offset = offset;
    AdvanceAndWatch();
    return reserved_offset;
}

void StreamBuffer::AdvanceAndWatch() {
    offset += mapped_size;
    const u64 tick = scheduler.CurrentTick();
    last_tick = tick;

    // Extend the last watch if it belongs to the same tick.
    if (current_watch_cursor != 0 && current_watches[current_watch_cursor - 1].tick == tick) {
        current_watches[current_watch_cursor - 1].upper_bound = offset;
        return;
    }

    if (current_watch_cursor + 1 >= current_watches.size()) {
        // Ensure that there are enough watches.
        ReserveWatches(current_watches, WATCHES_RESERVE_CHUNK);
    }

    auto& watch = current_watches[current_watch_cursor++];
    watch.upper_bound = offset;
    watch.tick = tick;
}

void StreamBuffer::ReserveWatches(std::vector<Watch>& watches, std::size_t grow_size) {
    watches.resize(watches.size() + grow_size);
}

bool StreamBuffer::WaitPendingOperations(u64 requested_upper_bound, bool allow_wait) {
    if (!invalidation_mark) {
        return true;
    }
    while (requested_upper_bound > wait_bound && wait_cursor < *invalidation_mark) {
        auto& watch = previous_watches[wait_cursor];
        if (!scheduler.IsFree(watch.tick) && !allow_wait) {
            return false;
        }
        scheduler.Wait(watch.tick);
        wait_bound = watch.upper_bound;
        ++wait_cursor;
    }
    return true;
}

} // namespace VideoCore
