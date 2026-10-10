// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <fstream>
#include <string>
#include <vector>
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"
#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/ir/microinstruction.h"
#include "video_core/renderer_vulkan/vk_common.h"
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE
using namespace Shader;using namespace Shader::Backend::SPIRV;
static constexpr u32 Words=65536,Bytes=Words*4;
static std::vector<u32> shader(bool robust) {
    Profile profile{};profile.supported_spirv=0x00010500;profile.emulate_robust_buffer_access=robust;
    Info info{};info.hw_stage=HwStage::Compute;info.sw_stage=SwStage::Compute;
    for(int i=0;i<3;++i) {
        auto& resource=info.buffers.emplace_back();resource.used_types=IR::Type::U32;resource.is_written=i==2;
        auto sharp=AmdGpu::Buffer::Placeholder(Bytes);sharp.data_format=u32(AmdGpu::DataFormat::Format32);
        std::memcpy(resource.sharp_fetch.immediates.data(),&sharp,sizeof(sharp));
    }
    RuntimeInfo runtime{};Backend::Bindings bindings{};EmitContext ctx(profile,runtime,info,bindings);
    ctx.AddCapability(spv::Capability::Int8);
    ctx.AddCapability(spv::Capability::Int16);
    ctx.AddCapability(spv::Capability::Int64);
    const auto gid=ctx.AddGlobalVariable(ctx.TypePointer(spv::StorageClass::Input,ctx.U32[3]),spv::StorageClass::Input);
    ctx.Decorate(gid,spv::Decoration::BuiltIn,u32(spv::BuiltIn::GlobalInvocationId));ctx.interfaces.push_back(gid);
    auto function=ctx.OpFunction(ctx.void_id,spv::FunctionControlMask::MaskNone,ctx.TypeFunction(ctx.void_id));
    ctx.AddLabel();ctx.DefineBufferProperties();
    auto thread=ctx.OpCompositeExtract(ctx.U32[1],ctx.OpLoad(ctx.U32[3],gid),0u);
    auto sum=ctx.u32_zero_value;
    for(u32 i=0;i<128;++i) {
        auto index=ctx.OpBitwiseAnd(ctx.U32[1],ctx.OpIAdd(ctx.U32[1],thread,ctx.ConstU32(i*37)),ctx.ConstU32(Words-1));
        auto address=index;
        if(robust) {
            IR::Inst inst(IR::Opcode::Void,0);IR::BufferInstInfo flags{};flags.voffset_enable.Assign(true);flags.address_shift.Assign(2);flags.access_bytes.Assign(4);inst.SetFlags(flags);
            address=EmitBufferAddress(ctx,&inst,0,ctx.u32_zero_value,ctx.OpShiftLeftLogical(ctx.U32[1],index,ctx.ConstU32(2u)),ctx.u32_zero_value);
        }
        auto value=EmitLoadBufferU32(ctx,nullptr,0,address);
        sum=ctx.OpBitwiseXor(ctx.U32[1],ctx.OpIMul(ctx.U32[1],sum,ctx.ConstU32(16777619u)),value);
    }
    EmitStoreBufferU32(ctx,nullptr,2,thread,sum);
    ctx.OpReturn();ctx.OpFunctionEnd();ctx.AddEntryPoint(spv::ExecutionModel::GLCompute,function,"main",std::span(ctx.interfaces));
    ctx.AddExecutionMode(function,spv::ExecutionMode::LocalSize,64u,1u,1u);
    return ctx.Assemble();
}
int main(int argc, char** argv) {
    if (argc > 2 && std::string(argv[2]) == "--emit-only") {
        for (u32 mode = 0; mode < 2; ++mode) {
            const auto code = shader(mode != 0);
            std::ofstream file(std::string(argv[1]) + "/benchmark-" + std::to_string(mode) + ".spv",
                               std::ios::binary);
            file.write(reinterpret_cast<const char*>(code.data()), code.size() * 4);
            assert(file.good());
        }
        std::puts("Bounds benchmark SPIR-V emitted without GPU execution");
        return 0;
    }

    std::fprintf(stderr, "buffer bounds test: Vulkan setup\n");
    vk::detail::DynamicLoader loader;
    VULKAN_HPP_DEFAULT_DISPATCHER.init(loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr"));
    const vk::ApplicationInfo app{.pApplicationName = "GCN buffer bounds semantics", .apiVersion = VK_API_VERSION_1_3};
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
    const vk::PushConstantRange push{.stageFlags=vk::ShaderStageFlagBits::eCompute,.offset=0,.size=sizeof(PushData)};
    auto pipeline_layout = device->createPipelineLayoutUnique({.setLayoutCount=1,.pSetLayouts=&*descriptors,
        .pushConstantRangeCount=1,.pPushConstantRanges=&push}).value;
    std::vector<u32> reference;
    auto queries=device->createQueryPoolUnique({.queryType=vk::QueryType::eTimestamp,.queryCount=2}).value;
    assert(physical.getProperties().limits.timestampComputeAndGraphics);
    const double timestamp_period=physical.getProperties().limits.timestampPeriod;
    std::array<vk::UniquePipeline, 2> pipelines;
    for (u32 mode = 0; mode < 2; ++mode) {
        auto code = shader(mode != 0);
        if (argc > 1) {
            std::ofstream file(std::string(argv[1]) + "/benchmark-" + std::to_string(mode) + ".spv",
                               std::ios::binary);
            file.write(reinterpret_cast<const char*>(code.data()), code.size() * 4);
            assert(file.good());
        }
        std::fprintf(stderr, "bounds benchmark: compile robust=%u\n", mode);
        auto module = device->createShaderModuleUnique({.codeSize=code.size()*4,.pCode=code.data()}).value;
        pipelines[mode] = device->createComputePipelineUnique({}, {
            .stage={.stage=vk::ShaderStageFlagBits::eCompute,.module=*module,.pName="main"},
            .layout=*pipeline_layout}).value;
        assert(pipelines[mode]);
    }
    for (u32 i = 0; i < Words; ++i) backing_data[i] = (i * 2654435761u) ^ 0x98765432u;
    std::array<std::vector<double>, 2> samples;
    std::ofstream raw;
    if (argc > 1) {
        raw.open(std::string(argv[1]) + "/timestamps.csv");
        assert(raw.good());
        raw << "round,mode,gpu_ms\n";
    }
    constexpr u32 Warmup = 8, Measured = 60;
    for (u32 repetition = 0; repetition < Warmup + Measured; ++repetition) {
        for (u32 pass = 0; pass < 2; ++pass) {
            const u32 mode = repetition % 2 == 0 ? pass : 1 - pass;
            command.resetQueryPool(*queries, 0, 2);
            command.bindPipeline(vk::PipelineBindPoint::eCompute, *pipelines[mode]);
            command.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *pipeline_layout, 0, set, {});
            command.writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe, *queries, 0);
            command.dispatch(Words / 64, 1, 1);
            command.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, *queries, 1);
            const vk::MemoryBarrier barrier{.srcAccessMask=vk::AccessFlagBits::eShaderWrite,
                                            .dstAccessMask=vk::AccessFlagBits::eHostRead};
            command.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
                                    vk::PipelineStageFlagBits::eHost, {}, barrier, {}, {});
            assert(command.end() == vk::Result::eSuccess);
            const vk::SubmitInfo submit{.commandBufferCount=1,.pCommandBuffers=&command};
            assert(queue.submit(submit) == vk::Result::eSuccess);
            assert(queue.waitIdle() == vk::Result::eSuccess);
            std::array<uint64_t, 2> ticks{};
            assert(device->getQueryPoolResults(*queries, 0, 2, sizeof(ticks), ticks.data(),
                sizeof(uint64_t), vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWait) ==
                vk::Result::eSuccess);
            const double ms = (ticks[1] - ticks[0]) * timestamp_period / 1e6;
            if (repetition >= Warmup) {
                samples[mode].push_back(ms);
                if (raw.is_open()) raw << repetition - Warmup << ',' << mode << ',' << ms << '\n';
            }
            if (reference.empty()) reference.assign(data, data + Words);
            else assert(std::equal(reference.begin(), reference.end(), data));
            device->freeCommandBuffers(*pool, command);
            command = device->allocateCommandBuffers({.commandPool=*pool,
                .level=vk::CommandBufferLevel::ePrimary,.commandBufferCount=1}).value.front();
            assert(command.begin(vk::CommandBufferBeginInfo{}) == vk::Result::eSuccess);
        }
    }
    for (u32 mode = 0; mode < 2; ++mode) {
        std::sort(samples[mode].begin(), samples[mode].end());
        const auto& values = samples[mode];
        const double median = (values[29] + values[30]) / 2;
        std::fprintf(stderr, "bounds benchmark: robust=%u median %.6fms IQR %.6f..%.6fms "
                     "(60 alternating samples, 8,388,608 loads, 65,536 invocations)\n",
                     mode, median, values[15], values[45]);
    }
    std::puts("Heavy compute checksum equivalence PASS");
}
