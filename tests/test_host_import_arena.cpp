// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <sirit/sirit.h>
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/host_import_arena.h"
#include <vk_mem_alloc.h>
#include "video_core/renderer_vulkan/vk_platform.h"

VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

struct Resource {
    u64 cpu_addr;
    u64 size_bytes;
    VideoCore::UniqueBuffer buffer;
    Resource(vk::PhysicalDevice physical, vk::Device device, VmaAllocator allocator, u64 address, u64 size,
             VideoCore::MemoryType type = VideoCore::MemoryType::DeviceLocal)
        : cpu_addr{address}, size_bytes{size}, buffer{device, allocator} {
        vk::BufferCreateInfo info{.size = size, .usage = VideoCore::AllFlags};
        if (type == VideoCore::MemoryType::ImportedHost) {
            buffer.ImportHost(physical, info, reinterpret_cast<void*>(address));
        } else {
            buffer.Create(info, type, nullptr);
        }
        assert(buffer.bda_addr);
    }
};

static std::vector<u32> ShaderCode(u64 table, u64 output, std::span<const u64> addresses,
                                   bool write) {
    Sirit::Module module(0x00010500);
    module.AddCapability(spv::Capability::Shader);
    module.AddCapability(spv::Capability::Int64);
    module.AddCapability(spv::Capability::PhysicalStorageBufferAddresses);
    module.AddExtension("SPV_KHR_physical_storage_buffer");
    module.SetMemoryModel(spv::AddressingModel::PhysicalStorageBuffer64, spv::MemoryModel::GLSL450);
    const auto void_type = module.TypeVoid();
    const auto u32_type = module.TypeInt(32, false);
    const auto u64_type = module.TypeInt(64, false);
    const auto ptr32 = module.TypePointer(spv::StorageClass::PhysicalStorageBuffer, u32_type);
    const auto ptr64 = module.TypePointer(spv::StorageClass::PhysicalStorageBuffer, u64_type);
    const auto function = module.OpFunction(void_type, spv::FunctionControlMask::MaskNone,
                                            module.TypeFunction(void_type));
    module.AddLabel(module.OpLabel());
    for (u32 i = 0; i < addresses.size(); ++i) {
        auto address = module.Constant(u64_type, addresses[i]);
        if (!write) {
            auto entry = module.OpIAdd(u64_type, module.Constant(u64_type, table),
                module.Constant(u64_type, (addresses[i] >> 14) * sizeof(u64)));
            auto bda = module.OpLoad(u64_type, module.OpConvertUToPtr(ptr64, entry),
                                     spv::MemoryAccessMask::Aligned, 8u);
            address = module.OpIAdd(u64_type, bda,
                                      module.Constant(u64_type, addresses[i] & 0x3fffu));
        }
        auto pointer = module.OpConvertUToPtr(ptr32, address);
        if (write) {
            module.OpStore(pointer, module.Constant(u32_type, 99u + 2u * i),
                           spv::MemoryAccessMask::Aligned, 4u);
        } else {
            auto value = module.OpLoad(u32_type, pointer, spv::MemoryAccessMask::Aligned, 4u);
            auto destination = module.OpConvertUToPtr(ptr32,
                module.Constant(u64_type, output + i * sizeof(u32)));
            module.OpStore(destination, value, spv::MemoryAccessMask::Aligned, 4u);
        }
    }
    module.OpReturn();
    module.OpFunctionEnd();
    module.AddEntryPoint(spv::ExecutionModel::GLCompute, function, "main", std::span<const Sirit::Id>{});
    module.AddExecutionMode(function, spv::ExecutionMode::LocalSize, 1u, 1u, 1u);
    return module.Assemble();
}

int main() {
    VideoCore::HostImportBackingMap backings;
    const std::array<VideoCore::HostImportBackingRange, 1> first{{{0x10000, 0x10000, 0x20000}}};
    assert(!backings.Check(0x10000, 0x10000, first));
    backings.Register(0x10000, 0x10000, {first.begin(), first.end()});
    const std::array<VideoCore::HostImportBackingRange, 1> alias{{{0x30000, 0x4000, 0x24000}}};
    const auto conflict = backings.Check(0x30000, 0x4000, alias);
    assert(conflict && conflict->existing.guest == 0x10000 && conflict->requested.backing == 0x24000);
    const std::array<VideoCore::HostImportBackingRange, 1> neighbor{{{0x30000, 0x10000, 0x30000}}};
    assert(!backings.Check(0x30000, 0x10000, neighbor));
    const std::array<VideoCore::HostImportBackingRange, 2> fragmented{{
        {0x10000, 0x8000, 0x20000}, {0x18000, 0x8000, 0x40000}}};
    assert(!backings.Check(0x10000, 0x10000, fragmented));
    backings.Register(0x10000, 0x10000, {fragmented.begin(), fragmented.end()});
    const std::array<VideoCore::HostImportBackingRange, 2> duplicate{{
        {0x10000, 0x8000, 0x20000}, {0x18000, 0x8000, 0x24000}}};
    assert(backings.Check(0x10000, 0x10000, duplicate));
    backings.EraseOverlapping(0x14000, 4);
    assert(!backings.Check(0x30000, 0x4000, alias));
    std::fprintf(stderr, "host import: Vulkan setup\n");
    vk::detail::DynamicLoader loader;
    VULKAN_HPP_DEFAULT_DISPATCHER.init(loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr"));
    const vk::ApplicationInfo app{.pApplicationName = "host-import arena address semantics", .apiVersion = VK_API_VERSION_1_3};
    const char* enumeration = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
    auto instance = vk::createInstanceUnique({.flags = vk::InstanceCreateFlagBits::eEnumeratePortabilityKHR,
        .pApplicationInfo = &app, .enabledExtensionCount = 1, .ppEnabledExtensionNames = &enumeration}).value;
    assert(instance);
    VULKAN_HPP_DEFAULT_DISPATCHER.init(*instance);
    const auto physical = instance->enumeratePhysicalDevices().value.at(0);
    const auto families = physical.getQueueFamilyProperties();
    u32 family = 0;
    while (!(families.at(family).queueFlags & vk::QueueFlagBits::eCompute)) ++family;
    const float priority = 1.0f;
    const vk::DeviceQueueCreateInfo queue_info{.queueFamilyIndex=family, .queueCount=1, .pQueuePriorities=&priority};
    const std::array<const char*, 2> extensions{"VK_KHR_portability_subset", VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME};
    const vk::PhysicalDeviceFeatures features{.shaderInt64=true};
    const vk::PhysicalDeviceVulkan12Features features12{.bufferDeviceAddress=true};
    auto device = physical.createDeviceUnique({.pNext=&features12, .queueCreateInfoCount=1,
        .pQueueCreateInfos=&queue_info, .enabledExtensionCount=extensions.size(), .ppEnabledExtensionNames=extensions.data(),
        .pEnabledFeatures=&features}).value;
    assert(device);
    VULKAN_HPP_DEFAULT_DISPATCHER.init(*device);
    const auto queue = device->getQueue(family, 0);
    const VmaVulkanFunctions functions{.vkGetInstanceProcAddr=VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr,
        .vkGetDeviceProcAddr=VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr};
    const VmaAllocatorCreateInfo info{.flags=VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT,
        .physicalDevice=physical, .device=*device, .pVulkanFunctions=&functions, .instance=*instance,
        .vulkanApiVersion=VK_API_VERSION_1_3};
    VmaAllocator allocator;
    assert(vmaCreateAllocator(&info, &allocator) == VK_SUCCESS);
    {
        constexpr u64 guest = 0x7000000000ULL;
        constexpr u64 page = 1u << 16;
        auto* backing = static_cast<u8*>(mmap(nullptr, 3*page, PROT_READ | PROT_WRITE,
            MAP_ANON | MAP_PRIVATE, -1, 0));
        assert(backing != MAP_FAILED);
        mach_vm_address_t guest_view = guest;
        vm_prot_t current, maximum;
        assert(mach_vm_remap(mach_task_self(), &guest_view, 3*page, 0, VM_FLAGS_FIXED,
            mach_task_self(), reinterpret_cast<uintptr_t>(backing), FALSE, &current, &maximum,
            VM_INHERIT_NONE) == KERN_SUCCESS);
        auto* guest_bytes = reinterpret_cast<u8*>(guest_view);
        assert(guest_bytes == reinterpret_cast<void*>(guest));
        mach_vm_address_t alias = guest + (1u << 24);
        assert(mach_vm_remap(mach_task_self(), &alias, 3*page, 0, VM_FLAGS_FIXED,
            mach_task_self(), reinterpret_cast<uintptr_t>(backing), FALSE, &current, &maximum,
            VM_INHERIT_NONE) == KERN_SUCCESS);
        assert(mprotect(guest_bytes, 3*page, PROT_READ) == 0);
        Resource output(physical, *device, allocator, 0, 64, VideoCore::MemoryType::HostCached);
        Resource table(physical, *device, allocator, 0, 128, VideoCore::MemoryType::HostCached);
        void* output_data;
        void* table_data;
        assert(vmaMapMemory(allocator, output.buffer.allocation, &output_data) == VK_SUCCESS);
        assert(vmaMapMemory(allocator, table.buffer.allocation, &table_data) == VK_SUCCESS);
        std::memset(output_data, 0xcc, 64);
        std::memset(table_data, 0, 128);
        auto pool = device->createCommandPoolUnique({.queueFamilyIndex=family}).value;
        auto command = device->allocateCommandBuffers({.commandPool=*pool,
            .level=vk::CommandBufferLevel::ePrimary, .commandBufferCount=1}).value.at(0);
        assert(command.begin(vk::CommandBufferBeginInfo{}) == vk::Result::eSuccess);
        auto barrier = [&] {
            const vk::MemoryBarrier memory{.srcAccessMask=vk::AccessFlagBits::eMemoryWrite,
                .dstAccessMask=vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite};
            command.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                                    vk::PipelineStageFlagBits::eAllCommands, {}, memory, {}, {});
        };
        std::vector<vk::UniqueShaderModule> modules;
        std::vector<vk::UniquePipeline> pipelines;
        auto layout = device->createPipelineLayoutUnique({}).value;
        auto dispatch = [&](std::span<const u64> addresses, bool write) {
            const u64 global_table = table.buffer.bda_addr - (guest >> 14) * sizeof(u64);
            auto code = ShaderCode(global_table, output.buffer.bda_addr, addresses, write);
            auto module = device->createShaderModuleUnique({.codeSize=code.size()*4, .pCode=code.data()}).value;
            auto pipeline = device->createComputePipelineUnique({}, {
                .stage={.stage=vk::ShaderStageFlagBits::eCompute, .module=*module, .pName="main"}, .layout=*layout}).value;
            assert(pipeline);
            command.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline);
            command.dispatch(1, 1, 1);
            barrier();
            modules.push_back(std::move(module));
            pipelines.push_back(std::move(pipeline));
        };
        VideoCore::HostImportArenaMap<Resource> arenas(16, 1u << 24);
        std::vector<std::unique_ptr<Resource>> retired;
        auto allocate = [&](u64 start, u64 size) {
            auto resource = std::make_unique<Resource>(physical, *device, allocator, start, size,
                                                       VideoCore::MemoryType::ImportedHost);
            return resource;
        };
        u32 completion_waits = 0;
        const vk::SubmitInfo submit{.commandBufferCount=1, .pCommandBuffers=&command};
        const auto complete = [&](u64 address, u64 size) {
            assert(address == guest && size == 3*page);
            assert(command.end() == vk::Result::eSuccess);
            assert(queue.submit(submit) == vk::Result::eSuccess);
            assert(queue.waitIdle() == vk::Result::eSuccess);
            assert(device->resetCommandPool(*pool) == vk::Result::eSuccess);
            assert(command.begin(vk::CommandBufferBeginInfo{}) == vk::Result::eSuccess);
            ++completion_waits;
        };
        u32 migrations = 0;
        auto migrate = [&](const Resource& from, const Resource& to) {
            assert(from.cpu_addr >= to.cpu_addr);
            // Both imported buffers address the same guest bytes; no migration copy.
            ++migrations;
        };
        auto retire = [&](std::unique_ptr<Resource> resource) { retired.push_back(std::move(resource)); };
        std::fprintf(stderr, "host import: allocate and GPU write\n");
        const auto* left = arenas.Obtain(guest + 4096, 8, allocate, migrate, retire, complete);
        const auto* right = arenas.Obtain(guest + 2*page + 4096, 8, allocate, migrate, retire, complete);
        assert(left->size_bytes == page && right->size_bytes == page);
        const std::array<u64, 2> gpu_write{left->buffer.bda_addr + 4100, right->buffer.bda_addr + 4100};
        dispatch(gpu_write, true);
        const std::array<u32, 1> value17{17}, value23{23};
        command.updateBuffer<u32>(left->buffer.buffer, 4096, value17);
        command.updateBuffer<u32>(right->buffer.buffer, 4096, value23);
        barrier();
        std::fprintf(stderr, "host import: grow with GPU completion and rebuild address table\n");
        const auto* combined = arenas.Obtain(guest + page - 4, page + 16,
                                            allocate, migrate, retire, complete);
        assert(completion_waits == 1);
        assert(combined->cpu_addr == guest && combined->size_bytes == 3*page);
        assert(migrations == 2 && retired.size() == 2);
        assert(arenas.Find(guest + 4096) == combined && arenas.Find(guest + 3*page - 1) == combined);
        assert(!arenas.Find(guest - 1) && !arenas.Find(guest + 3*page));
        assert(arenas.Obtain(guest + 4096, 8, allocate, migrate, retire, complete) == combined);
        auto* addresses = static_cast<u64*>(table_data);
        for (u32 block = 0; block < 12; ++block) {
            addresses[block] = combined->buffer.bda_addr + (u64(block) << 14);
        }
        assert(vmaFlushAllocation(allocator, table.buffer.allocation, 0, 128) == VK_SUCCESS);
        const std::array<u64, 6> reads{guest+4096, guest+4100, guest+2*page+4096,
            guest+2*page+4100, guest+page-4, guest+page};
        dispatch(reads, false);
        const vk::MemoryBarrier host{.srcAccessMask=vk::AccessFlagBits::eShaderWrite,
                                     .dstAccessMask=vk::AccessFlagBits::eHostRead};
        command.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
                                 vk::PipelineStageFlagBits::eHost, {}, host, {}, {});
        assert(command.end() == vk::Result::eSuccess);
        assert(queue.submit(submit) == vk::Result::eSuccess);
        assert(queue.waitIdle() == vk::Result::eSuccess);
        assert(vmaInvalidateAllocation(allocator, output.buffer.allocation, 0, 64) == VK_SUCCESS);
        const std::array<u32, 6> expected{17, 99, 23, 101, 0, 0};
        assert(std::memcmp(output_data, expected.data(), sizeof(expected)) == 0);
        assert(static_cast<u32*>(output_data)[6] == 0xcccccccc);
        assert(*reinterpret_cast<const u32*>(guest + 4100) == 99);
        assert(*reinterpret_cast<const u32*>(alias + 4100) == 99);
        assert(*reinterpret_cast<const u32*>(alias + 2*page + 4100) == 101);
        retired.clear();
        arenas.EraseOverlapping(guest + page, 4, [](auto resource) { resource.reset(); });
        assert(!arenas.Find(guest + 4096));
        assert(munmap(guest_bytes, 3*page) == 0);
        assert(mach_vm_deallocate(mach_task_self(), alias, 3*page) == KERN_SUCCESS);
        assert(munmap(backing, 3*page) == 0);
        backing = static_cast<u8*>(mmap(nullptr, 3*page, PROT_READ | PROT_WRITE,
            MAP_ANON | MAP_PRIVATE, -1, 0));
        assert(backing != MAP_FAILED);
        guest_view = guest;
        assert(mach_vm_remap(mach_task_self(), &guest_view, 3*page, 0, VM_FLAGS_FIXED,
            mach_task_self(), reinterpret_cast<uintptr_t>(backing), FALSE, &current, &maximum,
            VM_INHERIT_NONE) == KERN_SUCCESS);
        guest_bytes = reinterpret_cast<u8*>(guest_view);
        assert(device->resetCommandPool(*pool) == vk::Result::eSuccess);
        assert(command.begin(vk::CommandBufferBeginInfo{}) == vk::Result::eSuccess);
        const auto* fresh = arenas.Obtain(guest, 3*page, allocate, migrate, retire, complete);
        assert(fresh->buffer.bda_addr);
        for (u32 block = 0; block < 12; ++block) {
            addresses[block] = fresh->buffer.bda_addr + (u64(block) << 14);
        }
        assert(vmaFlushAllocation(allocator, table.buffer.allocation, 0, 128) == VK_SUCCESS);
        dispatch(reads, false);
        command.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
                                 vk::PipelineStageFlagBits::eHost, {}, host, {}, {});
        assert(command.end() == vk::Result::eSuccess);
        assert(queue.submit(submit) == vk::Result::eSuccess);
        assert(queue.waitIdle() == vk::Result::eSuccess);
        assert(vmaInvalidateAllocation(allocator, output.buffer.allocation, 0, 64) == VK_SUCCESS);
        for (u32 i = 0; i < 6; ++i) assert(static_cast<u32*>(output_data)[i] == 0);
        arenas.EraseOverlapping(guest, 3*page, [](auto resource) { resource.reset(); });
        assert(munmap(guest_bytes, 3*page) == 0);
        assert(munmap(backing, 3*page) == 0);
        vmaUnmapMemory(allocator, table.buffer.allocation);
        vmaUnmapMemory(allocator, output.buffer.allocation);
        std::puts("MoltenVK guest imports: real BDA, guest offsets, RO alias coherence, no-copy rebind and retire/zero/reimport PASS");
    }
    vmaDestroyAllocator(allocator);
}
