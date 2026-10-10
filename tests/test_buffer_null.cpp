// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"
#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/specialization.h"
#include "video_core/renderer_vulkan/vk_common.h"

VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE
using namespace Shader;
using namespace Shader::Backend::SPIRV;
static u32 ResultCount;

static std::vector<u32> shader(bool null_write) {
    Profile profile{};
    profile.supported_spirv = 0x00010500;
    profile.emulate_null_descriptors = true;
    Info info{};
    info.hw_stage = HwStage::Compute;
    info.sw_stage = SwStage::Compute;
    for (int i = 0; i < 3; ++i) {
        auto& resource = info.buffers.emplace_back();
        resource.is_written = true;
        resource.used_types = IR::Type::U32 | IR::Type::F32 | IR::Type::U8 | IR::Type::U16 | IR::Type::U64;
        auto sharp = AmdGpu::Buffer::Placeholder(256);
        sharp.base_address = (i != 0 || !null_write) ? 1 : 0;
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
    ctx.AddCapability(spv::Capability::StorageBuffer8BitAccess);
    ctx.AddCapability(spv::Capability::StorageBuffer16BitAccess);
    auto function = ctx.OpFunction(ctx.void_id, spv::FunctionControlMask::MaskNone,
                                   ctx.TypeFunction(ctx.void_id));
    ctx.AddLabel();
    auto address = ctx.ConstU32(0u);
    auto value = ctx.ConstU32(7u);
    EmitStoreBufferU32(ctx, nullptr, 0, address, value);
    std::vector<Sirit::Id> results;
    if (null_write) {
        assert(ctx.buffers[0].is_null && !ctx.buffers[1].is_null);
        auto add_vector = [&](Sirit::Id v, u32 n, bool fp = false) {
            for (u32 c=0;c<n;++c) {
                auto result = n == 1 ? v : ctx.OpCompositeExtract(fp ? ctx.F32[1] : ctx.U32[1], v, c);
                results.push_back(fp ? ctx.OpBitcast(ctx.U32[1], result) : result);
            }
        };
        add_vector(EmitReadConstBuffer(ctx, 0, address, ctx.u32_zero_value), 1);
        results.push_back(ctx.OpUConvert(ctx.U32[1], EmitLoadBufferU8(ctx, nullptr, 0, address)));
        results.push_back(ctx.OpUConvert(ctx.U32[1], EmitLoadBufferU16(ctx, nullptr, 0, address)));
        results.push_back(ctx.OpUConvert(ctx.U32[1], EmitLoadBufferU64(ctx, nullptr, 0, address)));
        add_vector(EmitLoadBufferU32(ctx, nullptr, 0, address), 1);
        add_vector(EmitLoadBufferU32x2(ctx, nullptr, 0, address), 2);
        add_vector(EmitLoadBufferU32x3(ctx, nullptr, 0, address), 3);
        add_vector(EmitLoadBufferU32x4(ctx, nullptr, 0, address), 4);
        add_vector(EmitLoadBufferF32(ctx, nullptr, 0, address), 1, true);
        add_vector(EmitLoadBufferF32x2(ctx, nullptr, 0, address), 2, true);
        add_vector(EmitLoadBufferF32x3(ctx, nullptr, 0, address), 3, true);
        add_vector(EmitLoadBufferF32x4(ctx, nullptr, 0, address), 4, true);
        auto v2=ctx.ConstantComposite(ctx.U32[2],value,value);
        auto v3=ctx.ConstantComposite(ctx.U32[3],value,value,value);
        auto v4=ctx.ConstantComposite(ctx.U32[4],value,value,value,value);
        auto f=ctx.ConstF32(7.f);
        EmitStoreBufferU8(ctx,nullptr,0,address,ctx.Constant(ctx.U8,7u));
        EmitStoreBufferU16(ctx,nullptr,0,address,ctx.Constant(ctx.U16,7u));
        EmitStoreBufferU64(ctx,nullptr,0,address,ctx.Constant(ctx.U64,7ull));
        EmitStoreBufferU32x2(ctx,nullptr,0,address,v2);
        EmitStoreBufferU32x3(ctx,nullptr,0,address,v3);
        EmitStoreBufferU32x4(ctx,nullptr,0,address,v4);
        EmitStoreBufferF32(ctx,nullptr,0,address,f);
        EmitStoreBufferF32x2(ctx,nullptr,0,address,ctx.ConstantComposite(ctx.F32[2],f,f));
        EmitStoreBufferF32x3(ctx,nullptr,0,address,ctx.ConstantComposite(ctx.F32[3],f,f,f));
        EmitStoreBufferF32x4(ctx,nullptr,0,address,ctx.ConstantComposite(ctx.F32[4],f,f,f,f));
        for (auto emit : {EmitBufferAtomicIAdd32, EmitBufferAtomicISub32,
                         EmitBufferAtomicSMin32, EmitBufferAtomicUMin32,
                         EmitBufferAtomicSMax32, EmitBufferAtomicUMax32,
                         EmitBufferAtomicAnd32, EmitBufferAtomicOr32,
                         EmitBufferAtomicXor32, EmitBufferAtomicSwap32}) {
            results.push_back(emit(ctx,nullptr,0,address,value));
        }
        results.push_back(EmitBufferAtomicInc32(ctx,nullptr,0,address,value));
        results.push_back(EmitBufferAtomicDec32(ctx,nullptr,0,address,value));
        results.push_back(EmitBufferAtomicCmpSwap32(ctx,nullptr,0,address,value,address));
        results.push_back(ctx.OpBitcast(ctx.U32[1],EmitBufferAtomicFMin32(ctx,nullptr,0,address,f)));
        results.push_back(ctx.OpBitcast(ctx.U32[1],EmitBufferAtomicFMax32(ctx,nullptr,0,address,f)));
        results.push_back(ctx.OpBitcast(ctx.U32[1],EmitBufferAtomicFCmpSwap32(ctx,nullptr,0,address,f,ctx.f32_zero_value)));
        for (auto emit : {EmitBufferAtomicIAdd64,EmitBufferAtomicSMin64,EmitBufferAtomicUMin64,
                         EmitBufferAtomicSMax64,EmitBufferAtomicUMax64}) {
            results.push_back(ctx.OpUConvert(ctx.U32[1],emit(ctx,nullptr,0,address,ctx.Constant(ctx.U64,7ull))));
        }
        ctx.input_params[0].is_null = true;
        for(u32 c=0;c<4;++c) results.push_back(ctx.OpBitcast(ctx.U32[1],EmitGetAttribute(ctx,IR::Attribute::Param0,c,0)));
    }
    ctx.OpMemoryBarrier(ctx.ConstU32(u32(spv::Scope::Device)),
        ctx.ConstU32(u32(spv::MemorySemanticsMask::UniformMemory | spv::MemorySemanticsMask::AcquireRelease)));
    results.push_back(EmitLoadBufferU32(ctx,nullptr,1,address));
    for (u32 i=0;i<results.size();++i) EmitStoreBufferU32(ctx,nullptr,2,ctx.ConstU32(i),results[i]);
    if(null_write) ResultCount=results.size();
    ctx.OpReturn();
    ctx.OpFunctionEnd();
    ctx.AddEntryPoint(spv::ExecutionModel::GLCompute,function,"main",std::span(ctx.interfaces));
    ctx.AddExecutionMode(function,spv::ExecutionMode::LocalSize,1u,1u,1u);
    return ctx.Assemble();
}

static void specialization_tests() {
    Info null_info{}, valid_info{};
    null_info.hw_stage = valid_info.hw_stage = HwStage::Compute;
    null_info.sw_stage = valid_info.sw_stage = SwStage::Compute;
    for (auto* info : {&null_info, &valid_info}) {
        auto& resource = info->buffers.emplace_back();
        auto sharp = AmdGpu::Buffer::Placeholder(256);
        sharp.base_address = info == &null_info ? 0 : 1;
        std::memcpy(resource.sharp_fetch.immediates.data(), &sharp, sizeof(sharp));
    }
    RuntimeInfo runtime{};
    Profile emulated{}, native{};
    emulated.emulate_null_descriptors = true;
    const StageSpecialization null_spec(null_info, runtime, emulated, {});
    const StageSpecialization valid_spec(valid_info, runtime, emulated, {});
    assert(!(null_spec == valid_spec) && !(valid_spec == null_spec));
    const StageSpecialization native_null(null_info, runtime, native, {});
    const StageSpecialization native_valid(valid_info, runtime, native, {});
    assert(native_null == native_valid && native_valid == native_null);
    VsAttribSpecialization a{}, b{};
    a.is_null = true;
    assert(a != b);
}

int main(int argc, char** argv) {
    specialization_tests();
    std::fprintf(stderr, "null buffer test: Vulkan setup\n");
    vk::detail::DynamicLoader loader;
    VULKAN_HPP_DEFAULT_DISPATCHER.init(loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr"));
    const vk::ApplicationInfo app{.pApplicationName = "null buffer semantics", .apiVersion = VK_API_VERSION_1_3};
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
    assert(command.begin(vk::CommandBufferBeginInfo{}) == vk::Result::eSuccess);
    auto backing = device->createBufferUnique({.size = 256, .usage = vk::BufferUsageFlagBits::eStorageBuffer}).value;
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
    auto backing_requirements = device->getBufferMemoryRequirements(*backing);
    assert(backing_requirements.memoryTypeBits & (1u << memory_type));
    auto backing_memory = device->allocateMemoryUnique({.allocationSize = backing_requirements.size,
                                                        .memoryTypeIndex = memory_type}).value;
    assert(device->bindBufferMemory(*backing, *backing_memory, 0) == vk::Result::eSuccess);
    auto backing_data = static_cast<u32*>(device->mapMemory(*backing_memory, 0, 256).value);
    std::fill_n(backing_data, 64, 0u);
    const std::array<vk::DescriptorSetLayoutBinding, 3> layout_bindings{{
        {.binding=0,.descriptorType=vk::DescriptorType::eStorageBuffer,.descriptorCount=1,.stageFlags=vk::ShaderStageFlagBits::eCompute},
        {.binding=1,.descriptorType=vk::DescriptorType::eStorageBuffer,.descriptorCount=1,.stageFlags=vk::ShaderStageFlagBits::eCompute},
        {.binding=2,.descriptorType=vk::DescriptorType::eStorageBuffer,.descriptorCount=1,.stageFlags=vk::ShaderStageFlagBits::eCompute}}};
    auto descriptors = device->createDescriptorSetLayoutUnique({.bindingCount=3,.pBindings=layout_bindings.data()}).value;
    const vk::DescriptorPoolSize size{vk::DescriptorType::eStorageBuffer,3};
    auto descriptor_pool = device->createDescriptorPoolUnique({.maxSets=1,.poolSizeCount=1,.pPoolSizes=&size}).value;
    auto set = device->allocateDescriptorSets({.descriptorPool=*descriptor_pool,.descriptorSetCount=1,.pSetLayouts=&*descriptors}).value.front();
    const vk::DescriptorBufferInfo dummy{.buffer=*backing,.offset=0,.range=256};
    const vk::DescriptorBufferInfo buffer{.buffer=*readback,.offset=0,.range=256};
    const std::array<vk::WriteDescriptorSet,3> writes{{
        {.dstSet=set,.dstBinding=0,.descriptorCount=1,.descriptorType=vk::DescriptorType::eStorageBuffer,.pBufferInfo=&dummy},
        {.dstSet=set,.dstBinding=1,.descriptorCount=1,.descriptorType=vk::DescriptorType::eStorageBuffer,.pBufferInfo=&dummy},
        {.dstSet=set,.dstBinding=2,.descriptorCount=1,.descriptorType=vk::DescriptorType::eStorageBuffer,.pBufferInfo=&buffer}}};
    device->updateDescriptorSets(writes, {});
    const vk::PushConstantRange push{.stageFlags=vk::ShaderStageFlagBits::eCompute,.offset=0,.size=128};
    auto pipeline_layout = device->createPipelineLayoutUnique({.setLayoutCount=1,.pSetLayouts=&*descriptors,
        .pushConstantRangeCount=1,.pPushConstantRanges=&push}).value;
    auto run = [&](bool null_write) {
        std::fprintf(stderr, "null buffer test: emit %d\n", null_write);
        std::fill_n(data, 64, 0xcafef00du);
        auto code = shader(null_write);
        if (argc > 1) {
            std::ofstream file(std::string(argv[1]) + (null_write ? "/null-buffer.spv" : "/valid-buffer.spv"), std::ios::binary);
            file.write(reinterpret_cast<const char*>(code.data()), code.size() * sizeof(u32));
            assert(file.good());
        }
        std::fprintf(stderr, "null buffer test: pipeline %d\n", null_write);
        auto module = device->createShaderModuleUnique({.codeSize=code.size()*4,.pCode=code.data()}).value;
        auto pipeline = device->createComputePipelineUnique({}, {
            .stage={.stage=vk::ShaderStageFlagBits::eCompute,.module=*module,.pName="main"},
            .layout=*pipeline_layout}).value;
        assert(pipeline);
        std::fprintf(stderr, "null buffer test: submit %d\n", null_write);
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
        std::fprintf(stderr, "null buffer test: readback %d\n", null_write);
        const u32 count = null_write ? ResultCount : 1;
        for (u32 i=0;i<count;++i) assert(data[i] == (null_write ? 0u : 7u));
        assert(data[count] == 0xcafef00du);
        assert(backing_data[0] == (null_write ? 0u : 7u));
        for (u32 i = 1; i < 64; ++i) assert(backing_data[i] == 0u);
    };
    run(true);
    assert(device->resetCommandPool(*pool) == vk::Result::eSuccess);
    assert(command.begin(vk::CommandBufferBeginInfo{}) == vk::Result::eSuccess);
    run(false);
    device->unmapMemory(*memory);
    device->unmapMemory(*backing_memory);
    backing.reset();
    readback.reset();
    std::puts("MoltenVK production buffer emitters: null loads/atomics/VS zero, stores discarded; shared backing unchanged; valid control PASS");
}
