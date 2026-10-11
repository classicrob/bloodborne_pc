// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"
#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "video_core/renderer_vulkan/vk_null_resources.h"

VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE
using namespace Shader;
using namespace Shader::Backend::SPIRV;

static std::vector<u32> shader(bool null_write) {
    Profile profile{};
    profile.supported_spirv = 0x00010500;
    profile.emulate_null_descriptors = true;
    Info info{};
    info.hw_stage = HwStage::Compute;
    info.sw_stage = SwStage::Compute;
    for (int i = 0; i < 2; ++i) {
        auto& resource = info.images.emplace_back();
        resource.is_written = true;
        auto sharp = AmdGpu::Image::Null(false);
        sharp.data_format = u64(AmdGpu::DataFormat::Format32_32_32_32);
        sharp.num_format = u64(AmdGpu::NumberFormat::Float);
        sharp.base_address = (i == 1 || !null_write) ? 1 : 0;
        resource.sharp_fetch.offsets.fill(0);
        resource.sharp_fetch.load_mask = 0;
        std::memcpy(resource.sharp_fetch.immediates.data(), &sharp, sizeof(sharp));
    }
    RuntimeInfo runtime{};
    Backend::Bindings bindings{};
    EmitContext ctx(profile, runtime, info, bindings);
    ctx.AddCapability(spv::Capability::Int8);
    ctx.AddCapability(spv::Capability::Int16);
    ctx.AddCapability(spv::Capability::Int64);
    ctx.AddCapability(spv::Capability::StorageImageReadWithoutFormat);
    ctx.AddCapability(spv::Capability::StorageImageWriteWithoutFormat);
    auto array = ctx.TypeRuntimeArray(ctx.U32[1]);
    ctx.Decorate(array, spv::Decoration::ArrayStride, 4u);
    auto structure = ctx.TypeStruct(array);
    ctx.Decorate(structure, spv::Decoration::Block);
    ctx.MemberDecorate(structure, 0, spv::Decoration::Offset, 0u);
    auto output = ctx.AddGlobalVariable(ctx.TypePointer(spv::StorageClass::StorageBuffer, structure),
                                        spv::StorageClass::StorageBuffer);
    ctx.Decorate(output, spv::Decoration::Binding, 2u);
    ctx.Decorate(output, spv::Decoration::DescriptorSet, 0u);
    ctx.interfaces.push_back(output);
    auto function = ctx.OpFunction(ctx.void_id, spv::FunctionControlMask::MaskNone,
                                   ctx.TypeFunction(ctx.void_id));
    ctx.AddLabel();
    const auto coords = ctx.ConstantNull(ctx.U32[2]);
    const auto value = ctx.ConstantComposite(ctx.F32[4], ctx.ConstF32(7.0f), ctx.ConstF32(7.0f),
                                             ctx.ConstF32(7.0f), ctx.ConstF32(7.0f));
    EmitImageWrite(ctx, nullptr, 0, coords, {}, {}, value);
    std::vector<Sirit::Id> results;
    auto add_vector = [&](Sirit::Id result, int count, bool integer = false) {
        for (int i = 0; i < count; ++i) {
            auto scalar = ctx.OpCompositeExtract(integer ? ctx.U32[1] : ctx.F32[1], result, u32(i));
            results.push_back(integer ? scalar : ctx.OpBitcast(ctx.U32[1], scalar));
        }
    };
    if (null_write) {
        assert(ctx.images[0].is_null && !ctx.images[1].is_null);
        add_vector(EmitImageSampleImplicitLod(ctx, nullptr, 0, coords, {}, {}), 4);
        add_vector(EmitImageSampleExplicitLod(ctx, nullptr, 0, coords, {}, {}), 4);
        add_vector(EmitImageSampleDrefImplicitLod(ctx, nullptr, 0, coords, {}, {}, {}), 4);
        add_vector(EmitImageSampleDrefExplicitLod(ctx, nullptr, 0, coords, {}, {}, {}), 4);
        add_vector(EmitImageGather(ctx, nullptr, 0, coords, {}), 4);
        add_vector(EmitImageGatherDref(ctx, nullptr, 0, coords, {}, {}), 4);
        add_vector(EmitImageGradient(ctx, nullptr, 0, coords, {}, {}, {}, {}), 4);
        add_vector(EmitImageRead(ctx, nullptr, 0, coords, {}, {}), 4);
        add_vector(EmitImageQueryDimensions(ctx, nullptr, 0, {}, true), 4, true);
        add_vector(EmitImageQueryLod(ctx, nullptr, 0, coords, {}), 2);
        results.push_back(EmitImageAtomicIAdd32(ctx, nullptr, 0, coords, ctx.ConstU32(9u)));
        results.push_back(EmitImageAtomicCmpSwap32(ctx, nullptr, 0, coords,
                                                 ctx.ConstU32(9u), ctx.ConstU32(0u)));
        results.push_back(ctx.OpBitcast(ctx.U32[1], EmitImageAtomicFMin32(
            ctx, nullptr, 0, coords, ctx.ConstF32(-1.0f))));
        results.push_back(ctx.OpBitcast(ctx.U32[1], EmitImageAtomicFMax32(
            ctx, nullptr, 0, coords, ctx.ConstF32(1.0f))));
    }
    // The same backing is also exposed through a valid descriptor. It must stay zero
    // after null writes/atomics, and become seven in the valid-write control shader.
    ctx.OpMemoryBarrier(ctx.ConstU32(u32(spv::Scope::Device)),
        ctx.ConstU32(u32(spv::MemorySemanticsMask::ImageMemory | spv::MemorySemanticsMask::AcquireRelease)));
    add_vector(EmitImageRead(ctx, nullptr, 1, coords, {}, {}), 4);
    auto pointer_type = ctx.TypePointer(spv::StorageClass::StorageBuffer, ctx.U32[1]);
    for (u32 i = 0; i < results.size(); ++i) {
        ctx.OpStore(ctx.OpAccessChain(pointer_type, output, ctx.ConstU32(0u), ctx.ConstU32(i)), results[i]);
    }
    ctx.OpReturn();
    ctx.OpFunctionEnd();
    ctx.AddEntryPoint(spv::ExecutionModel::GLCompute, function, "main", std::span(ctx.interfaces));
    ctx.AddExecutionMode(function, spv::ExecutionMode::LocalSize, 1u, 1u, 1u);
    return ctx.Assemble();
}

int main() {
    std::fprintf(stderr, "null test: Vulkan setup\n");
    vk::detail::DynamicLoader loader;
    VULKAN_HPP_DEFAULT_DISPATCHER.init(loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr"));
    const vk::ApplicationInfo app{.pApplicationName = "null descriptor semantics", .apiVersion = VK_API_VERSION_1_3};
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
    const vk::PhysicalDeviceVulkan12Features features12{.shaderInt8 = true};
    auto device = physical.createDeviceUnique({.pNext = &features12, .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_info, .enabledExtensionCount = 1,
        .ppEnabledExtensionNames = &subset, .pEnabledFeatures = &features}).value;
    assert(device);
    VULKAN_HPP_DEFAULT_DISPATCHER.init(*device);
    auto queue = device->getQueue(family, 0);
    auto pool = device->createCommandPoolUnique({.queueFamilyIndex = family}).value;
    auto command = device->allocateCommandBuffers({.commandPool = *pool,
        .level = vk::CommandBufferLevel::ePrimary, .commandBufferCount = 1}).value.front();
    assert(command.begin(vk::CommandBufferBeginInfo{}) == vk::Result::eSuccess);
    std::fprintf(stderr, "null test: placeholders\n");
    Vulkan::NullResources resources(physical, *device);
    auto initialize = [&](auto&& record) { record(command); };
    const auto view = resources.ImageView(vk::ImageViewType::e2D, vk::Format::eR32G32B32A32Sfloat,
                                          vk::SampleCountFlagBits::e1, true, initialize);
    // Exercise every view type and numeric class needed by the current SPIR-V emitter.
    for (auto type : {vk::ImageViewType::e1D, vk::ImageViewType::e1DArray, vk::ImageViewType::e2D,
                      vk::ImageViewType::e2DArray, vk::ImageViewType::e3D}) {
        for (auto format : {vk::Format::eR32G32B32A32Sfloat, vk::Format::eR32G32B32A32Sint,
                            vk::Format::eR32G32B32A32Uint}) {
            assert(resources.ImageView(type, format, vk::SampleCountFlagBits::e1, true, initialize));
        }
    }
    assert(resources.ImageView(vk::ImageViewType::e2D, vk::Format::eD32Sfloat,
                               vk::SampleCountFlagBits::e1, false, initialize));
    assert(resources.ImageView(vk::ImageViewType::e2D, vk::Format::eR32G32B32A32Sfloat,
                               vk::SampleCountFlagBits::e4, false, initialize));
    auto readback = device->createBufferUnique({.size = 256, .usage = vk::BufferUsageFlagBits::eStorageBuffer}).value;
    auto requirements = device->getBufferMemoryRequirements(*readback);
    auto memory_props = physical.getMemoryProperties();
    u32 memory_type = 0;
    const auto wanted = vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
    while (!(requirements.memoryTypeBits & (1u << memory_type)) ||
           (memory_props.memoryTypes[memory_type].propertyFlags & wanted) != wanted) ++memory_type;
    auto memory = device->allocateMemoryUnique({.allocationSize = requirements.size, .memoryTypeIndex = memory_type}).value;
    assert(device->bindBufferMemory(*readback, *memory, 0) == vk::Result::eSuccess);
    auto data = static_cast<u32*>(device->mapMemory(*memory, 0, 256).value);
    const std::array<vk::DescriptorSetLayoutBinding, 3> layout_bindings{{
        {.binding=0,.descriptorType=vk::DescriptorType::eStorageImage,.descriptorCount=1,.stageFlags=vk::ShaderStageFlagBits::eCompute},
        {.binding=1,.descriptorType=vk::DescriptorType::eStorageImage,.descriptorCount=1,.stageFlags=vk::ShaderStageFlagBits::eCompute},
        {.binding=2,.descriptorType=vk::DescriptorType::eStorageBuffer,.descriptorCount=1,.stageFlags=vk::ShaderStageFlagBits::eCompute}}};
    auto descriptors = device->createDescriptorSetLayoutUnique({.bindingCount=3,.pBindings=layout_bindings.data()}).value;
    const std::array<vk::DescriptorPoolSize, 2> sizes{{{vk::DescriptorType::eStorageImage,2},{vk::DescriptorType::eStorageBuffer,1}}};
    auto descriptor_pool = device->createDescriptorPoolUnique({.maxSets=1,.poolSizeCount=2,.pPoolSizes=sizes.data()}).value;
    auto set = device->allocateDescriptorSets({.descriptorPool=*descriptor_pool,.descriptorSetCount=1,.pSetLayouts=&*descriptors}).value.front();
    const vk::DescriptorImageInfo image{.imageView=view,.imageLayout=vk::ImageLayout::eGeneral};
    const vk::DescriptorBufferInfo buffer{.buffer=*readback,.offset=0,.range=256};
    const std::array<vk::WriteDescriptorSet,3> writes{{
        {.dstSet=set,.dstBinding=0,.descriptorCount=1,.descriptorType=vk::DescriptorType::eStorageImage,.pImageInfo=&image},
        {.dstSet=set,.dstBinding=1,.descriptorCount=1,.descriptorType=vk::DescriptorType::eStorageImage,.pImageInfo=&image},
        {.dstSet=set,.dstBinding=2,.descriptorCount=1,.descriptorType=vk::DescriptorType::eStorageBuffer,.pBufferInfo=&buffer}}};
    device->updateDescriptorSets(writes, {});
    const vk::PushConstantRange push{.stageFlags=vk::ShaderStageFlagBits::eCompute,.offset=0,.size=128};
    auto pipeline_layout = device->createPipelineLayoutUnique({.setLayoutCount=1,.pSetLayouts=&*descriptors,
        .pushConstantRangeCount=1,.pPushConstantRanges=&push}).value;
    auto run = [&](bool null_write) {
        std::fprintf(stderr, "null test: emit %d\n", null_write);
        std::fill_n(data, 64, 0xcafef00du);
        auto code = shader(null_write);
        std::fprintf(stderr, "null test: pipeline %d\n", null_write);
        auto module = device->createShaderModuleUnique({.codeSize=code.size()*4,.pCode=code.data()}).value;
        auto pipeline = device->createComputePipelineUnique({}, {
            .stage={.stage=vk::ShaderStageFlagBits::eCompute,.module=*module,.pName="main"},
            .layout=*pipeline_layout}).value;
        assert(pipeline);
        std::fprintf(stderr, "null test: submit %d\n", null_write);
        command.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline);
        command.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *pipeline_layout, 0, set, {});
        command.dispatch(1,1,1);
        const vk::MemoryBarrier barrier{.srcAccessMask=vk::AccessFlagBits::eShaderWrite,
            .dstAccessMask=vk::AccessFlagBits::eHostRead};
        command.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
                                vk::PipelineStageFlagBits::eHost, {}, barrier, {}, {});
        assert(command.end() == vk::Result::eSuccess);
        const vk::SubmitInfo submit{.commandBufferCount=1,.pCommandBuffers=&command};
        assert(queue.submit(submit) == vk::Result::eSuccess);
        assert(queue.waitIdle() == vk::Result::eSuccess);
        std::fprintf(stderr, "null test: readback %d\n", null_write);
        const u32 count = null_write ? 46 : 4;
        for (u32 i=0;i<count;++i) assert(data[i] == (null_write ? 0u : 0x40e00000u));
        assert(data[count] == 0xcafef00du);
    };
    run(true);
    assert(device->resetCommandPool(*pool) == vk::Result::eSuccess);
    assert(command.begin(vk::CommandBufferBeginInfo{}) == vk::Result::eSuccess);
    run(false);
    device->unmapMemory(*memory);
    readback.reset();
    std::puts("MoltenVK production image emitters: null reads/query/atomics zero, writes discarded; valid control PASS");
}
