// SPDX-License-Identifier: GPL-2.0-or-later
// Match the release dylib's Vulkan-Hpp layout, then enable this test's assertions.
#define NDEBUG
#include "video_core/renderer_vulkan/vk_common.h"
#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <vector>
#include <unistd.h>
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"
#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "test_alarm.h"
using namespace Shader;
using namespace Shader::Backend::SPIRV;
static constexpr u32 Words = 4096, Bytes = Words * 4;
static std::vector<u32> shader(bool decrement) {
    Profile profile{};
    profile.supported_spirv = 0x00010500;
    profile.emulate_robust_buffer_access = true;
    Info info{};
    info.hw_stage = HwStage::Compute;
    info.sw_stage = SwStage::Compute;
    for (int i = 0; i < 3; ++i) {
        auto& resource = info.buffers.emplace_back();
        resource.used_types = IR::Type::U32;
        resource.is_written = i != 1;
        const auto sharp = AmdGpu::Buffer::Placeholder(Bytes);
        std::memcpy(resource.sharp_fetch.immediates.data(), &sharp, sizeof(sharp));
    }
    RuntimeInfo runtime{};
    Backend::Bindings bindings{};
    EmitContext ctx(profile, runtime, info, bindings);
    ctx.AddCapability(spv::Capability::Int8);
    ctx.AddCapability(spv::Capability::Int16);
    ctx.AddCapability(spv::Capability::Int64);
    const auto gid = ctx.AddGlobalVariable(ctx.TypePointer(spv::StorageClass::Input, ctx.U32[3]),
                                           spv::StorageClass::Input);
    ctx.Decorate(gid, spv::Decoration::BuiltIn, u32(spv::BuiltIn::GlobalInvocationId));
    ctx.interfaces.push_back(gid);
    const auto function = ctx.OpFunction(ctx.void_id, spv::FunctionControlMask::MaskNone,
                                         ctx.TypeFunction(ctx.void_id));
    ctx.AddLabel();
    ctx.DefineBufferProperties();
    const auto thread = ctx.OpCompositeExtract(ctx.U32[1], ctx.OpLoad(ctx.U32[3], gid), 0u);
    const auto limit = EmitLoadBufferU32(ctx, nullptr, 1, ctx.ConstU32(1u));
    const auto old = decrement
        ? EmitBufferAtomicDec32(ctx, nullptr, 0, ctx.u32_zero_value, limit)
        : EmitBufferAtomicInc32(ctx, nullptr, 0, ctx.u32_zero_value, limit);
    EmitStoreBufferU32(ctx, nullptr, 2, thread, old);
    ctx.OpReturn();
    ctx.OpFunctionEnd();
    ctx.AddEntryPoint(spv::ExecutionModel::GLCompute, function, "main", std::span(ctx.interfaces));
    ctx.AddExecutionMode(function, spv::ExecutionMode::LocalSize, 64u, 1u, 1u);
    return ctx.Assemble();
}
int main(int argc, char** argv) {

    assert(argc == 2);
    test_alarm(45);
    std::fprintf(stderr, "atomic contention test: Vulkan setup\n");
    vk::detail::DynamicLoader loader;
    VULKAN_HPP_DEFAULT_DISPATCHER.init(loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr"));
    const vk::ApplicationInfo app{.pApplicationName = "GCN bounded atomic contention", .apiVersion = VK_API_VERSION_1_3};
    const char* enumeration = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
    auto instance = vk::createInstanceUnique({.flags = vk::InstanceCreateFlagBits::eEnumeratePortabilityKHR,
        .pApplicationInfo = &app, .enabledExtensionCount = 1, .ppEnabledExtensionNames = &enumeration}).value;
    assert(instance);
    VULKAN_HPP_DEFAULT_DISPATCHER.init(*instance);
    auto physical = instance->enumeratePhysicalDevices().value.front();
    auto families = physical.getQueueFamilyProperties();
    u32 family = 0;
    while (!(families.at(family).queueFlags & vk::QueueFlagBits::eCompute)) ++family;
    float priority = 1.0f;
    const vk::DeviceQueueCreateInfo queue_info{.queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority};
    const char* subset = "VK_KHR_portability_subset";
    const vk::PhysicalDeviceFeatures features{.shaderStorageImageReadWithoutFormat = true,
        .shaderStorageImageWriteWithoutFormat = true, .shaderInt64 = true, .shaderInt16 = true};
    vk::PhysicalDeviceVulkan12Features features12{.storageBuffer8BitAccess = true, .shaderInt8 = true};
    const vk::PhysicalDeviceVulkan11Features features11{.pNext = &features12, .storageBuffer16BitAccess = true};
    auto device = physical.createDeviceUnique({.pNext = &features11, .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_info, .enabledExtensionCount = 1,
        .ppEnabledExtensionNames = &subset, .pEnabledFeatures = &features}).value;
    assert(device);
    VULKAN_HPP_DEFAULT_DISPATCHER.init(*device);
    auto queue = device->getQueue(family, 0);
    auto pool = device->createCommandPoolUnique({.queueFamilyIndex = family}).value;
    auto command = device->allocateCommandBuffers({.commandPool = *pool,
        .level = vk::CommandBufferLevel::ePrimary, .commandBufferCount = 1}).value.front();
    auto backing = device->createBufferUnique({.size = Bytes, .usage = vk::BufferUsageFlagBits::eStorageBuffer}).value;
    auto readback = device->createBufferUnique({.size = Bytes, .usage = vk::BufferUsageFlagBits::eStorageBuffer}).value;
    auto requirements = device->getBufferMemoryRequirements(*readback);
    auto memory_props = physical.getMemoryProperties();
    u32 memory_type = 0;
    const auto wanted = vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
    while (!(requirements.memoryTypeBits & (1u << memory_type)) ||
           (memory_props.memoryTypes[memory_type].propertyFlags & wanted) != wanted) ++memory_type;
    auto memory = device->allocateMemoryUnique({ .allocationSize = requirements.size, .memoryTypeIndex = memory_type}).value;
    assert(device->bindBufferMemory(*readback, *memory, 0) == vk::Result::eSuccess);
    auto data = static_cast<u32*>(device->mapMemory(*memory, 0, Bytes).value);
    auto backing_requirements = device->getBufferMemoryRequirements(*backing);
    assert(backing_requirements.memoryTypeBits & (1u << memory_type));
    auto backing_memory = device->allocateMemoryUnique({ .allocationSize = backing_requirements.size,
                                                        .memoryTypeIndex = memory_type}).value;
    assert(device->bindBufferMemory(*backing, *backing_memory, 0) == vk::Result::eSuccess);
    auto backing_data = static_cast<u32*>(device->mapMemory(*backing_memory, 0, Bytes).value);
    std::fill_n(backing_data, Words, 0u);
    const std::array<vk::DescriptorSetLayoutBinding, 3> layout_bindings{{
        {.binding=0,.descriptorType=vk::DescriptorType::eStorageBuffer,.descriptorCount=1,.stageFlags=vk::ShaderStageFlagBits::eCompute},
        {.binding=1,.descriptorType=vk::DescriptorType::eStorageBuffer,.descriptorCount=1,.stageFlags=vk::ShaderStageFlagBits::eCompute},
        {.binding=2,.descriptorType=vk::DescriptorType::eStorageBuffer,.descriptorCount=1,.stageFlags=vk::ShaderStageFlagBits::eCompute}}};
    auto descriptors = device->createDescriptorSetLayoutUnique({.bindingCount=3,.pBindings=layout_bindings.data()}).value;
    const vk::DescriptorPoolSize size{vk::DescriptorType::eStorageBuffer,3};
    auto descriptor_pool = device->createDescriptorPoolUnique({.maxSets=1,.poolSizeCount=1,.pPoolSizes=&size}).value;
    auto set = device->allocateDescriptorSets({.descriptorPool=*descriptor_pool,.descriptorSetCount=1,.pSetLayouts=&*descriptors}).value.front();
    const vk::DescriptorBufferInfo dummy{.buffer=*backing,.offset=0,.range=Bytes};
    const vk::DescriptorBufferInfo buffer{.buffer=*readback,.offset=0,.range=Bytes};
    const std::array<vk::WriteDescriptorSet,3> writes{{
        {.dstSet=set,.dstBinding=0,.descriptorCount=1,.descriptorType=vk::DescriptorType::eStorageBuffer,.pBufferInfo=&dummy},
        {.dstSet=set,.dstBinding=1,.descriptorCount=1,.descriptorType=vk::DescriptorType::eStorageBuffer,.pBufferInfo=&dummy},
        {.dstSet=set,.dstBinding=2,.descriptorCount=1,.descriptorType=vk::DescriptorType::eStorageBuffer,.pBufferInfo=&buffer}}};
    device->updateDescriptorSets(writes, {});
    const vk::PushConstantRange push{.stageFlags=vk::ShaderStageFlagBits::eCompute,.offset=0,.size=sizeof(Shader::PushData)};
    auto pipeline_layout = device->createPipelineLayoutUnique({.setLayoutCount=1,.pSetLayouts=&*descriptors,
        .pushConstantRangeCount=1,.pPushConstantRanges=&push}).value;
    const std::array<std::pair<u32, u32>, 8> cases{{
        {0, UINT32_MAX}, {UINT32_MAX - 1, UINT32_MAX}, {99, 5}, {0, 5},
        {1, 1}, {0, 0}, {5, 5}, {UINT32_MAX, 0},
    }};
    for (bool decrement : {false, true}) {
        const auto code = shader(decrement);
        const auto path = std::string(argv[1]) + (decrement ? "/dec.spv" : "/inc.spv");
        std::ofstream file(path, std::ios::binary);
        file.write(reinterpret_cast<const char*>(code.data()), code.size() * sizeof(u32));
        assert(file); file.close();
        const auto module = device->createShaderModuleUnique({.codeSize = code.size()*4, .pCode = code.data()}).value;
        const auto pipeline = device->createComputePipelineUnique({}, {
            .stage={.stage=vk::ShaderStageFlagBits::eCompute, .module=*module, .pName="main"},
            .layout=*pipeline_layout}).value;
        assert(pipeline);
        for (const auto [initial, limit] : cases) {
            std::fill_n(backing_data, Words, 0x12345678u);
            backing_data[0] = initial;
            backing_data[1] = limit;
            std::fill_n(data, Words, 0xdeadbeefu);
            assert(command.begin(vk::CommandBufferBeginInfo{}) == vk::Result::eSuccess);
            const vk::MemoryBarrier before{.srcAccessMask=vk::AccessFlagBits::eHostWrite,
                .dstAccessMask=vk::AccessFlagBits::eShaderRead|vk::AccessFlagBits::eShaderWrite};
            command.pipelineBarrier(vk::PipelineStageFlagBits::eHost,
                vk::PipelineStageFlagBits::eComputeShader, {}, before, {}, {});
            command.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline);
            command.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *pipeline_layout, 0, set, {});
            command.dispatch(Words / 64, 1, 1);
            const vk::MemoryBarrier after{.srcAccessMask=vk::AccessFlagBits::eShaderWrite,
                .dstAccessMask=vk::AccessFlagBits::eHostRead};
            command.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
                vk::PipelineStageFlagBits::eHost, {}, after, {}, {});
            assert(command.end() == vk::Result::eSuccess);
            const vk::SubmitInfo submit{.commandBufferCount=1, .pCommandBuffers=&command};
            assert(queue.submit(submit) == vk::Result::eSuccess);
            assert(queue.waitIdle() == vk::Result::eSuccess);
            std::map<u32, unsigned> expected, actual;
            u32 counter = initial;
            for (u32 i = 0; i < Words; ++i) {
                ++expected[counter];
                counter = decrement ? ((counter == 0 || counter > limit) ? limit : counter - 1)
                                    : (counter >= limit ? 0 : counter + 1);
                ++actual[data[i]];
            }
            assert(actual == expected);
            assert(backing_data[0] == counter && backing_data[1] == limit);
            for (u32 i = 2; i < Words; ++i) assert(backing_data[i] == 0x12345678u);
            std::printf("PASS %s initial=%u limit=%u final=%u, 4096 old values exact\n",
                        decrement ? "DEC" : "INC", initial, limit, counter);
            device->freeCommandBuffers(*pool, command);
            command = device->allocateCommandBuffers({.commandPool=*pool,
                .level=vk::CommandBufferLevel::ePrimary, .commandBufferCount=1}).value.front();
        }
    }
    device->unmapMemory(*memory);
    device->unmapMemory(*backing_memory);
    backing.reset(); readback.reset();
    std::puts("Production atomic CAS contention PASS: 64 workgroups, 16 edge/wrap scenarios");
}
