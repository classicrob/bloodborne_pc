// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef NDEBUG
#define NDEBUG
#define BB_RESTORE_TEST_ASSERTS
#endif
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_resource_pool.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include <sirit/sirit.h>
#ifdef BB_RESTORE_TEST_ASSERTS
#undef NDEBUG
#endif
#include <cassert>
#include <cstdio>

static PFN_vkCreateGraphicsPipelines create_pipelines;
static unsigned checked_pipelines;

static VKAPI_ATTR VkResult VKAPI_CALL CheckInputs(VkDevice device, VkPipelineCache cache,
    uint32_t count, const VkGraphicsPipelineCreateInfo* infos,
    const VkAllocationCallbacks* allocator, VkPipeline* pipelines) {
    for (uint32_t i = 0; i < count; ++i) {
        const auto& info = infos[i];
        assert(info.pVertexInputState);
        assert(info.pVertexInputState->vertexBindingDescriptionCount == 0);
        assert(info.pVertexInputState->vertexAttributeDescriptionCount == 0);
        for (uint32_t state = 0; state < info.pDynamicState->dynamicStateCount; ++state) {
            assert(info.pDynamicState->pDynamicStates[state] != VK_DYNAMIC_STATE_VERTEX_INPUT_EXT);
            assert(info.pDynamicState->pDynamicStates[state] != VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE);
        }
        ++checked_pipelines;
    }
    return create_pipelines(device, cache, count, infos, allocator, pipelines);
}

static std::vector<u32> VertexShader() {
    Sirit::Module m(0x00010500);
    m.AddCapability(spv::Capability::Shader);
    const auto f = m.TypeFloat(32), v4 = m.TypeVector(f, 4);
    const auto position = m.AddGlobalVariable(m.TypePointer(spv::StorageClass::Output, v4),
                                             spv::StorageClass::Output);
    m.Decorate(position, spv::Decoration::BuiltIn, spv::BuiltIn::Position);
    const auto fn = m.OpFunction(m.TypeVoid(), spv::FunctionControlMask::MaskNone,
                                 m.TypeFunction(m.TypeVoid()));
    m.AddLabel(m.OpLabel());
    const auto zero = m.Constant(f, 0.0f), one = m.Constant(f, 1.0f);
    m.OpStore(position, m.OpCompositeConstruct(v4, zero, zero, zero, one));
    m.OpReturn();
    m.OpFunctionEnd();
    m.AddEntryPoint(spv::ExecutionModel::Vertex, fn, "main", std::array{position});
    m.SetMemoryModel(spv::AddressingModel::Logical, spv::MemoryModel::GLSL450);
    return m.Assemble();
}

int main() {
    std::fprintf(stderr, "Vertex pulling: production pipeline setup\n");
    Vulkan::Instance instance(0, false);
    Vulkan::Scheduler scheduler(instance);
    const std::array pools{vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer, 32}};
    Vulkan::DescriptorHeap heap(instance, scheduler.GetWorkSemaphore(), pools);
    Shader::Profile profile{};
    profile.emulate_vertex_fetch = true;
    Shader::Info vs{};
    vs.hw_stage = Shader::HwStage::Vertex;
    vs.sw_stage = Shader::SwStage::Vertex;
    const auto code = VertexShader();
    const auto shader = instance.GetDevice().createShaderModuleUnique(
        {.codeSize = code.size() * sizeof(u32), .pCode = code.data()}).value;
    assert(shader);
    std::array<const Shader::Info*, Vulkan::MaxShaderStages> stages{};
    std::array<Shader::RuntimeInfo, Vulkan::MaxShaderStages> runtimes{};
    std::array<vk::ShaderModule, Vulkan::MaxShaderStages> modules{};
    stages[u32(Shader::SwStage::Vertex)] = &vs;
    modules[u32(Shader::SwStage::Vertex)] = *shader;
    Shader::Gcn::FetchShaderData fetch{};
    fetch.attributes.push_back({.sgpr_base = 255});
    Vulkan::GraphicsPipelineKey key{};
    key.prim_type = AmdGpu::PrimitiveType::TriangleList;
    key.polygon_mode = AmdGpu::PolygonMode::Fill;
    key.num_samples = 1;
    key.depth_samples = 1;
    create_pipelines = VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateGraphicsPipelines;
    VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateGraphicsPipelines = CheckInputs;
    for (bool preload : {false, true}) {
        Vulkan::GraphicsPipeline::SerializationSupport data{};
        data.vertex_attributes.push_back({0, 0, vk::Format::eR32Sfloat, 0});
        data.vertex_bindings.push_back({0, 4, vk::VertexInputRate::eVertex});
        data.divisors.push_back({0, 2});
        data.multisampling.rasterizationSamples = vk::SampleCountFlagBits::e1;
        Vulkan::GraphicsPipeline pipeline(instance, scheduler, heap, profile, key, {},
            stages, runtimes, fetch, modules, data, preload);
        assert(pipeline.Handle() && pipeline.UsesVertexPulling());
        assert(data.vertex_attributes.empty() && data.vertex_bindings.empty() && data.divisors.empty());
        Vulkan::VertexInputs<vk::VertexInputAttributeDescription> attributes;
        Vulkan::VertexInputs<vk::VertexInputBindingDescription> bindings;
        Vulkan::VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT> divisors;
        Vulkan::VertexInputs<AmdGpu::Buffer> buffers;
        pipeline.GetVertexInputs(attributes, bindings, divisors, buffers, 1, 1);
        assert(attributes.empty() && bindings.empty() && divisors.empty() && buffers.empty());
    }
    VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateGraphicsPipelines = create_pipelines;
    assert(checked_pipelines == 2);
    std::printf("Vertex pulling: live/preloaded production layouts PASS (device dynamic input=%u)\n",
                instance.IsVertexInputDynamicState());
}
