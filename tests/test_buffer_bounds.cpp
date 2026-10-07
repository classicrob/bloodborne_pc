// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"
#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/ir/microinstruction.h"
#include "video_core/renderer_vulkan/vk_common.h"

VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE
using namespace Shader;
using namespace Shader::Backend::SPIRV;
struct Fixture {u32 size,prefix,stride,records;bool swizzle,invalid;};
static const Fixture fixtures[]={{32,4,0,32,false,false},{32,4,8,4,false,false},
    {128,4,16,8,true,false},{32,4,0,32,false,false},{32,4,0,32,false,true},{32,3,0,32,false,false}};
static std::vector<u32> expected;
static std::vector<u32> shader(int scenario) {
    Profile profile{};
    profile.supported_spirv=0x00010500;
    profile.emulate_robust_buffer_access=true;
    Info info{};
    info.hw_stage=HwStage::Compute;
    info.sw_stage=SwStage::Compute;
    auto fixture=fixtures[scenario];
    for(int i=0;i<3;++i) {
        auto& resource=info.buffers.emplace_back();
        resource.is_written=true;
        resource.used_types=IR::Type::U32|IR::Type::F32|IR::Type::U8|IR::Type::U16|IR::Type::U64;
        auto sharp=AmdGpu::Buffer::Placeholder(i==2?256:fixture.size);
        sharp.base_address=1;
        sharp.data_format=u32(AmdGpu::DataFormat::Format32);
        if(i<2) {
            sharp.stride=fixture.stride;sharp.num_records=fixture.records;
            sharp.swizzle_enable=fixture.swizzle;sharp.element_size=1;sharp.index_stride=0;
            if(fixture.invalid) sharp.data_format=0;
        }
        resource.sharp_fetch.load_mask=0;
        std::memcpy(resource.sharp_fetch.immediates.data(),&sharp,sizeof(sharp));
    }
    RuntimeInfo runtime{};Backend::Bindings bindings{};
    EmitContext ctx(profile,runtime,info,bindings);
    ctx.AddCapability(spv::Capability::Int8);ctx.AddCapability(spv::Capability::Int16);
    ctx.AddCapability(spv::Capability::Int64);ctx.AddCapability(spv::Capability::StorageBuffer8BitAccess);
    ctx.AddCapability(spv::Capability::StorageBuffer16BitAccess);
    auto function=ctx.OpFunction(ctx.void_id,spv::FunctionControlMask::MaskNone,ctx.TypeFunction(ctx.void_id));
    ctx.AddLabel();
    ctx.DefineBufferProperties();
    for(u32 handle=0;handle<2;++handle)
        for(u32 shift=0;shift<4;++shift)
            ctx.buffers[handle].offsets[shift]=ctx.ConstU32(fixture.prefix>>shift);
    std::vector<Sirit::Id> results;expected.clear();
    auto put=[&](Sirit::Id value,u32 want) {results.push_back(value);expected.push_back(want);};
    auto address=[&](u32 index,u32 offset,u32 soffset=0,u32 shift=2,bool formatted=false,u32 bytes=4) {
        IR::Inst inst(IR::Opcode::Void,0);
        IR::BufferInstInfo flags{};flags.index_enable.Assign(fixture.stride!=0);
        flags.voffset_enable.Assign(true);flags.address_shift.Assign(shift);
        flags.access_bytes.Assign(bytes);flags.typed.Assign(formatted);inst.SetFlags(flags);
        return EmitBufferAddress(ctx,&inst,0,ctx.ConstU32(index),ctx.ConstU32(offset),ctx.ConstU32(soffset));
    };
    auto load=[&](u32 index,u32 offset,u32 want,u32 soffset=0) {
        put(EmitLoadBufferU32(ctx,nullptr,0,address(index,offset,soffset)),want);
    };
    if(scenario==0) {
        load(0,0,0x1001);load(0,28,0x1008);load(0,32,0);load(0,6,0x1002);
        load(0,UINT32_MAX,0);load(0,28,0,4);
        auto a=address(0,28);auto v=EmitLoadBufferU32x4(ctx,nullptr,0,a);
        for(u32 c=0;c<4;++c) put(ctx.OpCompositeExtract(ctx.U32[1],v,c),c==0?0x1008:0);
        put(ctx.OpUConvert(ctx.U32[1],EmitLoadBufferU64(ctx,nullptr,0,a)),0x1008);
        put(ctx.OpUConvert(ctx.U32[1],EmitLoadBufferU8(ctx,nullptr,0,address(0,32,0,0,false,1))),0);
        put(ctx.OpUConvert(ctx.U32[1],EmitLoadBufferU16(ctx,nullptr,0,address(0,32,0,1,false,2))),0);
        const auto dead=ctx.ConstU32(0xdeadbeefu);
        EmitStoreBufferU32x4(ctx,nullptr,0,a,ctx.ConstantComposite(ctx.U32[4],dead,dead,dead,dead));
        EmitStoreBufferU8(ctx,nullptr,0,address(0,32,0,0,false,1),ctx.Constant(ctx.U8,7u));
        EmitStoreBufferU16(ctx,nullptr,0,address(0,32,0,1,false,2),ctx.Constant(ctx.U16,7u));
        EmitStoreBufferU64(ctx,nullptr,0,address(0,32),ctx.Constant(ctx.U64,7ull));
        put(EmitBufferAtomicIAdd32(ctx,nullptr,0,address(0,32),ctx.ConstU32(1u)),0);
        put(EmitBufferAtomicCmpSwap32(ctx,nullptr,0,address(0,32),dead,dead),0);
        put(EmitBufferAtomicIAdd32(ctx,nullptr,0,address(0,24),ctx.ConstU32(1u)),0x1007);
        put(EmitReadConstBuffer(ctx,0,ctx.ConstU32(8u),ctx.ConstU32(0xabcdu)),0xabcd);
    } else if(scenario==1) {
        load(0,0,0x1001);load(3,4,0x1008);load(4,0,0);load(1,8,0);
        auto a=address(0,4);auto v=EmitLoadBufferU32x2(ctx,nullptr,0,a);
        put(ctx.OpCompositeExtract(ctx.U32[1],v,0),0x1002);
        put(ctx.OpCompositeExtract(ctx.U32[1],v,1),0);
        put(EmitReadConstBuffer(ctx,0,ctx.ConstU32(9u),ctx.ConstU32(0xabcdu)),0xabcd);
    } else if(scenario==2) {
        load(1,0,0x1002);load(0,4,0x1009);load(8,0,0);load(0,16,0);
    } else if(scenario==3) {
        auto a=address(0,20,0,2,true,16);auto v=EmitLoadBufferU32x4(ctx,nullptr,0,a);
        for(u32 c=0;c<4;++c) put(ctx.OpCompositeExtract(ctx.U32[1],v,c),0);
        auto seven=ctx.ConstU32(7u);
        EmitStoreBufferU32x4(ctx,nullptr,0,a,ctx.ConstantComposite(ctx.U32[4],seven,seven,seven,seven));
        a=address(0,16,0,2,true,16);v=EmitLoadBufferU32x4(ctx,nullptr,0,a);
        for(u32 c=0;c<4;++c) put(ctx.OpCompositeExtract(ctx.U32[1],v,c),0x1005+c);
        put(ctx.OpSelect(ctx.U32[1],ctx.OpINotEqual(ctx.U1[1],address(0,28,0,2,true,16),ctx.ConstU32(UINT32_MAX)),ctx.ConstU32(1u),ctx.u32_zero_value),0);
    } else if(scenario==4) {
        load(0,0,0);put(EmitReadConstBuffer(ctx,0,ctx.ConstU32(0u),ctx.ConstU32(0xabcdu)),0x1001);
        EmitStoreBufferU32(ctx,nullptr,0,address(0,0),ctx.ConstU32(7u));
        put(EmitBufferAtomicIAdd32(ctx,nullptr,0,address(0,0),ctx.ConstU32(1u)),0);
    } else {
        load(0,0,0x1000);load(0,1,0x1001);load(0,29,0);load(0,32,0);
    }
    for(u32 i=0;i<results.size();++i) EmitStoreBufferU32(ctx,nullptr,2,ctx.ConstU32(i),results[i]);
    ctx.OpReturn();ctx.OpFunctionEnd();
    ctx.AddEntryPoint(spv::ExecutionModel::GLCompute,function,"main",std::span(ctx.interfaces));
    ctx.AddExecutionMode(function,spv::ExecutionMode::LocalSize,1u,1u,1u);
    return ctx.Assemble();
}
int main(int argc, char** argv) {

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
    for (int scenario=0;scenario<6;++scenario) {
        std::fprintf(stderr, "buffer bounds test: emit %d\n", scenario);
        const auto fixture = fixtures[scenario];
        const vk::DescriptorBufferInfo guest{.buffer=*backing,.offset=0,.range=fixture.prefix+fixture.size};
        auto case_writes = writes;
        case_writes[0].pBufferInfo = case_writes[1].pBufferInfo = &guest;
        device->updateDescriptorSets(case_writes, {});
        for (u32 i=0;i<64;++i) backing_data[i]=0x1000u+i;
        std::fill_n(data,64,0xcafef00du);
        auto code=shader(scenario);
        if(argc>1) {
            std::ofstream file(std::string(argv[1])+"/bounds-"+std::to_string(scenario)+".spv",std::ios::binary);
            file.write(reinterpret_cast<const char*>(code.data()), code.size()*4);
            assert(file.good());
        }
        std::fprintf(stderr,"buffer bounds test: pipeline %d\n",scenario);
        auto module=device->createShaderModuleUnique({.codeSize=code.size()*4,.pCode=code.data()}).value;
        auto pipeline=device->createComputePipelineUnique({}, {
            .stage={.stage=vk::ShaderStageFlagBits::eCompute,.module=*module,.pName="main"},
            .layout=*pipeline_layout}).value;
        assert(pipeline);
        command.bindPipeline(vk::PipelineBindPoint::eCompute,*pipeline);
        command.bindDescriptorSets(vk::PipelineBindPoint::eCompute,*pipeline_layout,0,set,{});
        command.dispatch(1,1,1);
        const vk::MemoryBarrier barrier{.srcAccessMask=vk::AccessFlagBits::eShaderWrite,
            .dstAccessMask=vk::AccessFlagBits::eHostRead};
        command.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,vk::PipelineStageFlagBits::eHost,{},barrier,{},{});
        assert(command.end()==vk::Result::eSuccess);
        const vk::SubmitInfo submit{.commandBufferCount=1,.pCommandBuffers=&command};
        assert(queue.submit(submit)==vk::Result::eSuccess);
        assert(queue.waitIdle()==vk::Result::eSuccess);
        for(u32 i=0;i<expected.size();++i) {
            if(data[i]!=expected[i]) std::fprintf(stderr,"case%d result%d: got%#x expected%#x\n",scenario,i,data[i],expected[i]);
            assert(data[i]==expected[i]);
        }
        for(u32 i=0;i<64;++i) {
            u32 wanted=0x1000u+i;
            if(scenario==0 && i==8) wanted=0xdeadbeefu;
            if(scenario==0 && i==7) ++wanted;
            if(backing_data[i]!=wanted) std::fprintf(stderr,"case%d backing%d: got%#x expected%#x\n",scenario,i,backing_data[i],wanted);
            assert(backing_data[i]==wanted);
        }
        std::fprintf(stderr,"buffer bounds test: case%d PASS\n",scenario);
        device->freeCommandBuffers(*pool,command);
        command=device->allocateCommandBuffers({.commandPool=*pool,.level=vk::CommandBufferLevel::ePrimary,.commandBufferCount=1}).value.front();
        assert(command.begin(vk::CommandBufferBeginInfo{})==vk::Result::eSuccess);
    }
    std::puts("MoltenVK production buffer bounds: guest/raw/structured/swizzle/typed/invalid/unaligned, stores+atomics+canaries PASS");
}
