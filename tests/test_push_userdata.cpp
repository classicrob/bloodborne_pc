// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"
#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "video_core/renderer_vulkan/vk_common.h"

VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE
using namespace Shader;
using namespace Shader::Backend::SPIRV;
static std::vector<u32> expected;

static PushData Pack() {
    PushData push{};
    Backend::Bindings bindings{};
    std::array<u32, NUM_USER_DATA_REGS> regs{};
    Info info{};
    info.user_data = regs;
    info.ud_mask.mask = 0xffff;
    for (u32 stage = 0; stage < NUM_PUSH_USER_DATA_REGS / NUM_USER_DATA_REGS; ++stage) {
        for (u32 reg = 0; reg < regs.size(); ++reg) regs[reg] = 0x10000000 + 0x100 * stage + reg;
        info.PushUd(bindings, push);
    }
    assert(bindings.user_data == NUM_PUSH_USER_DATA_REGS);
    for (u32 byte = 0; byte < NUM_BUFFERS; ++byte) push.AddOffset(byte, byte + 7);
    push.motion_param = 0xabc01234;
    push.scene_size = 0x87654321;
    push.xoffset = 2.0f; push.yoffset = 3.0f; push.xscale = 4.0f; push.yscale = 5.0f;
#ifdef __APPLE__
    PushData pair{};
    bindings = {};
    info.ud_mask.mask = 0xfff;
    info.PushUd(bindings, pair);
    info.ud_mask.mask = 0x300f;
    info.PushUd(bindings, pair);
    assert(bindings.user_data == 18);
    assert(pair.ud_regs[16] == regs[12] && pair.ud_regs[17] == regs[13]);
#endif
    return push;
}

static std::vector<u32> ShaderCode(const PushData& push) {
    Profile profile{};
    profile.supported_spirv = 0x00010500;
    Info info{};
    info.hw_stage = HwStage::Compute; info.sw_stage = SwStage::Compute;
    info.ud_mask.mask = 0xffff;
    auto& resource = info.buffers.emplace_back();
    resource.used_types = IR::Type::U32;
    resource.is_written = true;
    auto sharp = AmdGpu::Buffer::Placeholder(512);
    sharp.base_address = 1;
    std::memcpy(resource.sharp_fetch.immediates.data(), &sharp, sizeof(sharp));
    RuntimeInfo runtime{};
    Backend::Bindings bindings{};
    EmitContext ctx(profile, runtime, info, bindings);
    ctx.AddCapability(spv::Capability::Int64);
    ctx.AddCapability(spv::Capability::Int8);
    ctx.AddCapability(spv::Capability::Int16);
    const auto function = ctx.OpFunction(ctx.void_id, spv::FunctionControlMask::MaskNone,
                                         ctx.TypeFunction(ctx.void_id));
    ctx.AddLabel();
    std::vector<Sirit::Id> results;
    expected.clear();
    for (u32 stage = 0; stage < NUM_PUSH_USER_DATA_REGS / NUM_USER_DATA_REGS; ++stage) {
        bindings.user_data = stage * NUM_USER_DATA_REGS;
        for (u32 reg = 0; reg < NUM_USER_DATA_REGS; ++reg) {
            results.push_back(EmitGetUserData(ctx, IR::ScalarReg(reg)));
            expected.push_back(push.ud_regs[bindings.user_data + reg]);
        }
    }
    const auto word = [&](u32 member, u32 component, u32 want) {
        const auto ptr = ctx.OpAccessChain(ctx.TypePointer(spv::StorageClass::PushConstant, ctx.U32[1]),
                                          ctx.push_data_block, ctx.ConstU32(member), ctx.ConstU32(component));
        results.push_back(ctx.OpLoad(ctx.U32[1], ptr)); expected.push_back(want);
    };
    for (u32 i = 0; i < NUM_BUFFERS / 4; ++i) {
        u32 want; std::memcpy(&want, push.buf_offsets.data() + i * 4, sizeof(want));
        word(PushData::BufOffsetIndex + i / 4, i % 4, want);
    }
    for (const auto [member, want] : {std::pair{PushData::MotionParamIndex, push.motion_param},
                                    std::pair{PushData::SceneSizeIndex, push.scene_size}}) {
        const auto ptr = ctx.OpAccessChain(ctx.TypePointer(spv::StorageClass::PushConstant, ctx.U32[1]),
                                          ctx.push_data_block, ctx.ConstU32(member));
        results.push_back(ctx.OpLoad(ctx.U32[1], ptr)); expected.push_back(want);
    }
    for (u32 i = 0; i < 4; ++i) {
        const auto ptr = ctx.OpAccessChain(ctx.TypePointer(spv::StorageClass::PushConstant, ctx.F32[1]),
                                          ctx.push_data_block, ctx.ConstU32(i));
        results.push_back(ctx.OpBitcast(ctx.U32[1], ctx.OpLoad(ctx.F32[1], ptr)));
        u32 want; std::memcpy(&want, reinterpret_cast<const u8*>(&push) + 4 * i, sizeof(want));
        expected.push_back(want);
    }
    for (u32 i = 0; i < results.size(); ++i) EmitStoreBufferU32(ctx, nullptr, 0, ctx.ConstU32(i), results[i]);
    ctx.OpReturn(); ctx.OpFunctionEnd();
    ctx.AddEntryPoint(spv::ExecutionModel::GLCompute, function, "main", std::span(ctx.interfaces));
    ctx.AddExecutionMode(function, spv::ExecutionMode::LocalSize, 1u, 1u, 1u);
    return ctx.Assemble();
}
static void CompileCapacity(u32 start, u32 mask) {
    Info info{};
    info.hw_stage = HwStage::Compute; info.sw_stage = SwStage::Compute;
    info.pgm_hash = 0x52738b8a;
    info.ud_mask.mask = mask;
    Common::ObjectPool<IR::Inst> pool;
    IR::Block block(pool);
    IR::IREmitter ir(block);
    ir.Prologue(); ir.Epilogue();
    IR::Program program(info);
    program.blocks.push_back(&block);
    program.syntax_list.push_back({.data={.block=&block}, .type=IR::AbstractSyntaxNode::Type::Block});
    program.syntax_list.push_back({.type=IR::AbstractSyntaxNode::Type::Return});
    RuntimeInfo runtime{};
    runtime.Initialize(HwStage::Compute, SwStage::Compute);
    runtime.hw.cs.workgroup_size = {1, 1, 1};
    Profile profile{};
    profile.supported_spirv = 0x00010500;
    Backend::Bindings binding{.user_data=start};
    const auto code = EmitSPIRV(profile, runtime, program, binding);
    assert(!code.empty());
    assert(binding.user_data == start + std::popcount(mask));
}

static void CheckCapacity() {
    CompileCapacity(NUM_PUSH_USER_DATA_REGS - 16, 0xffff);
#ifdef __APPLE__
    CompileCapacity(12, 0x300f);
    for (const auto [start, mask] : {std::pair{NUM_PUSH_USER_DATA_REGS + 1, 1u},
                                   std::pair{NUM_PUSH_USER_DATA_REGS - 4, 0xffu},
                                   std::pair{0u, 0x10000u}}) {
        int output[2]; assert(pipe(output) == 0);
        std::fflush(nullptr);
        const pid_t child = fork(); assert(child >= 0);
        if (!child) {
            close(output[0]); assert(dup2(output[1], STDERR_FILENO) >= 0); close(output[1]);
            CompileCapacity(start, mask); _Exit(0);
        }
        close(output[1]);
        std::string message;
        char bytes[1024]; ssize_t count;
        while ((count = read(output[0], bytes, sizeof(bytes))) > 0) message.append(bytes, count);
        assert(count == 0); close(output[0]);
        int status; assert(waitpid(child, &status, 0) == child);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 23);
        assert(message.find("Apple shader user-data push capacity exceeded") != std::string::npos);
        assert(message.find("shader=0x52738b8a") != std::string::npos);
    }
#endif
    std::puts("PASS pre-emission capacity boundary, 12+6 stage pair, overflow and invalid SGPR rejection");
}

int main(int argc, char** argv) {

    CheckCapacity();
    std::fprintf(stderr, "push user-data test: Vulkan setup\n");
    vk::detail::DynamicLoader loader;
    VULKAN_HPP_DEFAULT_DISPATCHER.init(loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr"));
    const vk::ApplicationInfo app{.pApplicationName = "GCN packed user-data semantics", .apiVersion = VK_API_VERSION_1_3};
    const char* enumeration = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
    auto instance = vk::createInstanceUnique({.flags = vk::InstanceCreateFlagBits::eEnumeratePortabilityKHR,
        .pApplicationInfo = &app, .enabledExtensionCount = 1, .ppEnabledExtensionNames = &enumeration}).value;
    assert(instance);
    VULKAN_HPP_DEFAULT_DISPATCHER.init(*instance);
    auto physical = instance->enumeratePhysicalDevices().value.front();
    assert(physical.getProperties().limits.maxPushConstantsSize >= sizeof(PushData));
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

    auto backing = device->createBufferUnique({.size = 512, .usage = vk::BufferUsageFlagBits::eStorageBuffer}).value;
    auto readback = device->createBufferUnique({.size = 512, .usage = vk::BufferUsageFlagBits::eStorageBuffer}).value;
    auto requirements = device->getBufferMemoryRequirements(*readback);
    auto memory_props = physical.getMemoryProperties();
    u32 memory_type = 0;
    const auto wanted = vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
    while (!(requirements.memoryTypeBits & (1u << memory_type)) ||
           (memory_props.memoryTypes[memory_type].propertyFlags & wanted) != wanted) ++memory_type;
    auto memory = device->allocateMemoryUnique({.allocationSize = requirements.size, .memoryTypeIndex = memory_type}).value;
    assert(device->bindBufferMemory(*readback, *memory, 0) == vk::Result::eSuccess);
    auto data = static_cast<u32*>(device->mapMemory(*memory, 0, 512).value);
    auto backing_requirements = device->getBufferMemoryRequirements(*backing);
    assert(backing_requirements.memoryTypeBits & (1u << memory_type));
    auto backing_memory = device->allocateMemoryUnique({.allocationSize = backing_requirements.size,
                                                        .memoryTypeIndex = memory_type}).value;
    assert(device->bindBufferMemory(*backing, *backing_memory, 0) == vk::Result::eSuccess);
    auto backing_data = static_cast<u32*>(device->mapMemory(*backing_memory, 0, 512).value);
    std::fill_n(backing_data, 128, 0u);
    const std::array<vk::DescriptorSetLayoutBinding, 3> layout_bindings{{
        {.binding=0,.descriptorType=vk::DescriptorType::eStorageBuffer,.descriptorCount=1,.stageFlags=vk::ShaderStageFlagBits::eCompute},
        {.binding=1,.descriptorType=vk::DescriptorType::eStorageBuffer,.descriptorCount=1,.stageFlags=vk::ShaderStageFlagBits::eCompute},
        {.binding=2,.descriptorType=vk::DescriptorType::eStorageBuffer,.descriptorCount=1,.stageFlags=vk::ShaderStageFlagBits::eCompute}}};
    auto descriptors = device->createDescriptorSetLayoutUnique({.bindingCount=3,.pBindings=layout_bindings.data()}).value;
    const vk::DescriptorPoolSize size{vk::DescriptorType::eStorageBuffer,3};
    auto descriptor_pool = device->createDescriptorPoolUnique({.maxSets=1,.poolSizeCount=1,.pPoolSizes=&size}).value;
    auto set = device->allocateDescriptorSets({.descriptorPool=*descriptor_pool,.descriptorSetCount=1,.pSetLayouts=&*descriptors}).value.front();
    const vk::DescriptorBufferInfo dummy{.buffer=*backing,.offset=0,.range=512};
    const vk::DescriptorBufferInfo buffer{.buffer=*readback,.offset=0,.range=512};
    const std::array<vk::WriteDescriptorSet,3> writes{{
        {.dstSet=set,.dstBinding=0,.descriptorCount=1,.descriptorType=vk::DescriptorType::eStorageBuffer,.pBufferInfo=&dummy},
        {.dstSet=set,.dstBinding=1,.descriptorCount=1,.descriptorType=vk::DescriptorType::eStorageBuffer,.pBufferInfo=&dummy},
        {.dstSet=set,.dstBinding=2,.descriptorCount=1,.descriptorType=vk::DescriptorType::eStorageBuffer,.pBufferInfo=&buffer}}};
    device->updateDescriptorSets(writes, {});
    const vk::PushConstantRange push{.stageFlags=vk::ShaderStageFlagBits::eCompute,.offset=0,.size=sizeof(PushData)};
    auto pipeline_layout = device->createPipelineLayoutUnique({.setLayoutCount=1,.pSetLayouts=&*descriptors,
        .pushConstantRangeCount=1,.pPushConstantRanges=&push}).value;
    const auto push_data = Pack();
    const auto code = ShaderCode(push_data);
    if (argc > 1) {
        std::ofstream file(argv[1], std::ios::binary);
        file.write(reinterpret_cast<const char*>(code.data()), code.size() * 4);
        assert(file.good());
    }
    auto module = device->createShaderModuleUnique({.codeSize=code.size()*4,.pCode=code.data()}).value;
    auto pipeline = device->createComputePipelineUnique({}, {
        .stage={.stage=vk::ShaderStageFlagBits::eCompute,.module=*module,.pName="main"},
        .layout=*pipeline_layout}).value;
    assert(pipeline);
    auto output_write = writes[0]; output_write.pBufferInfo = &buffer;
    device->updateDescriptorSets(output_write, {});
    for (u32 run = 0; run < 3; ++run) {
        std::fill_n(data, 128, 0xcafef00du);
        assert(command.begin(vk::CommandBufferBeginInfo{}) == vk::Result::eSuccess);
        command.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline);
        command.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *pipeline_layout, 0, set, {});
        command.pushConstants(*pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0,
                              sizeof(push_data), &push_data);
        command.dispatch(1, 1, 1);
        const vk::MemoryBarrier barrier{.srcAccessMask=vk::AccessFlagBits::eShaderWrite,
                                       .dstAccessMask=vk::AccessFlagBits::eHostRead};
        command.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eHost,
                                {}, barrier, {}, {});
        assert(command.end() == vk::Result::eSuccess);
        const vk::SubmitInfo submit{.commandBufferCount=1,.pCommandBuffers=&command};
        assert(queue.submit(submit) == vk::Result::eSuccess);
        assert(queue.waitIdle() == vk::Result::eSuccess);
        for (u32 i = 0; i < expected.size(); ++i) {
            if (data[i] != expected[i]) std::fprintf(stderr, "word %u got %#x expected %#x\n", i, data[i], expected[i]);
            assert(data[i] == expected[i]);
        }
        for (u32 i = expected.size(); i < 128; ++i) assert(data[i] == 0xcafef00du);
        std::printf("PASS packed UD %u slots + all following push fields + canaries, run %u\n",
                    NUM_PUSH_USER_DATA_REGS, run);
        device->freeCommandBuffers(*pool, command);
        command = device->allocateCommandBuffers({.commandPool=*pool,
            .level=vk::CommandBufferLevel::ePrimary,.commandBufferCount=1}).value.front();
    }
    device->unmapMemory(*memory); device->unmapMemory(*backing_memory);
}
