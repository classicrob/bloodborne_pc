// SPDX-License-Identifier: GPL-2.0-or-later
// Stage-interface regression for RectList/QuadList emulation (and the direct VS->FS path).
//
// GCN lets a pixel shader read a parameter index the vertex shader never exported. Vulkan does not:
// every Input location of a stage must be written by the previous stage (VUID-RuntimeSpirv-
// OpEntryPoint-08743). On MoltenVK the auxiliary RectList TCS reading an unwritten location drew
// nothing at all. This fixture renders through the production auxiliary TCS/TES emitter and reads
// pixels back: each FS-read location must arrive as the VS value when written and zero when not.
#include <vulkan/vulkan.h>
#include <sirit/sirit.h>
#include "shader_recompiler/backend/spirv/emit_spirv_quad_rect.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/runtime_info.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        const VkResult r_ = (x);                                                                   \
        if (r_ != VK_SUCCESS) {                                                                    \
            std::fprintf(stderr, "FATAL %s:%d %s -> %d\n", __FILE__, __LINE__, #x, int(r_));       \
            std::exit(2);                                                                          \
        }                                                                                          \
    } while (0)

namespace {

constexpr u32 Size = 16;
constexpr float Clear = -7.0f;

using Shader::Backend::SPIRV::AuxShaderType;
using Shader::Backend::SPIRV::EmitAuxilaryTessShader;

#ifndef BB_RECT_IFACE_BASELINE
// Producer and consumer masks must equal what EmitContext declares: parameters as stored, motion
// 30/31 only from a hardware vertex shader with motion on (EmitContext::VertexMotion), and from the
// FS only when it has motion enabled.
u32 CheckMasks() {
    using Shader::HwStage;
    using Shader::SwStage;
    u32 failures = 0;
    const auto expect = [&](const char* what, u32 got, u32 want) {
        const bool ok = got == want;
        std::fprintf(stderr, "mask: %-46s %#010x %s\n", what, got, ok ? "ok" : "MISMATCH");
        failures += ok ? 0 : 1;
    };
    const auto producer = [](HwStage hw, SwStage sw, u32 params, bool motion) {
        Shader::Info info{};
        info.hw_stage = hw;
        info.sw_stage = sw;
        for (u32 i = 0; i < 32; ++i) {
            if (params & (1U << i)) {
                info.stores.Set(Shader::IR::Attribute::Param0 + i, 0);
            }
        }
        Shader::RuntimeInfo runtime{};
        runtime.hw.vs.motion_vectors = motion;
        return Shader::Backend::SPIRV::PreRasterOutputLocations(info, runtime);
    };
    constexpr u32 Motion = 3U << 30;
    Shader::MotionVectors::positions_address = 0x1000;
    expect("VS params 0,2", producer(HwStage::Vertex, SwStage::Vertex, 0b101, false), 0b101);
    expect("VS motion on", producer(HwStage::Vertex, SwStage::Vertex, 0b1, true), Motion | 1);
    expect("TES with hw.vs.motion_vectors set: no motion",
           producer(HwStage::Vertex, SwStage::TessellationEval, 0b11, true), 0b11);
    expect("ES (hw Export) with motion flag: no motion",
           producer(HwStage::Export, SwStage::Vertex, 0b1, true), 0b1);
    Shader::MotionVectors::positions_address = 0;
    expect("VS motion flag, no positions buffer: no motion",
           producer(HwStage::Vertex, SwStage::Vertex, 0b1, true), 0b1);
    Shader::HwFragmentRuntimeInfo fs{};
    fs.num_inputs = 3;
    fs.inputs[0] = {.param_index = 4};
    fs.inputs[1] = {.param_index = 9, .is_default = true}; // default value, not an input
    fs.inputs[2] = {.param_index = 9, .is_default = true, .is_flat = true}; // flat passthrough
    expect("FS inputs: default skipped, flat kept", Shader::Backend::SPIRV::AuxiliaryVaryingLocations(fs), 1U << 4 | 1U << 9);
    fs.motion_vectors = true;
    expect("FS motion on", Shader::Backend::SPIRV::AuxiliaryVaryingLocations(fs), Motion | 1U << 4 | 1U << 9);
    return failures;
}
#endif

float VsValue(u32 location, u32 comp) {
    return float(location + 1) + 0.125f * float(comp + 1);
}

// Vertex shader: positions from gl_VertexIndex (RectList: three corners; QuadList: four corners
// in ring order; direct: one big triangle), and a constant vec4 per written location so
// interpolation is exact.
std::vector<u32> VertexShader(u32 written_mask, AmdGpu::PrimitiveType prim) {
    Sirit::Module m(0x00010500);
    m.AddCapability(spv::Capability::Shader);
    const auto f32 = m.TypeFloat(32), int_t = m.TypeInt(32, true), b = m.TypeBool();
    const auto v4 = m.TypeVector(f32, 4);
    std::vector<Sirit::Id> interfaces;
    const auto position = m.AddGlobalVariable(m.TypePointer(spv::StorageClass::Output, v4),
                                              spv::StorageClass::Output);
    m.Decorate(position, spv::Decoration::BuiltIn, spv::BuiltIn::Position);
    interfaces.push_back(position);
    const auto vertex_index = m.AddGlobalVariable(m.TypePointer(spv::StorageClass::Input, int_t),
                                                  spv::StorageClass::Input);
    m.Decorate(vertex_index, spv::Decoration::BuiltIn, spv::BuiltIn::VertexIndex);
    interfaces.push_back(vertex_index);
    std::vector<std::pair<u32, Sirit::Id>> outputs;
    for (u32 loc = 0; loc < 32; ++loc) {
        if (!(written_mask & (1u << loc))) {
            continue;
        }
        const auto out = m.AddGlobalVariable(m.TypePointer(spv::StorageClass::Output, v4),
                                             spv::StorageClass::Output);
        m.Decorate(out, spv::Decoration::Location, loc);
        interfaces.push_back(out);
        outputs.emplace_back(loc, out);
    }
    const auto fn = m.OpFunction(m.TypeVoid(), spv::FunctionControlMask::MaskNone,
                                 m.TypeFunction(m.TypeVoid()));
    m.AddLabel(m.OpLabel());
    const auto idx = m.OpLoad(int_t, vertex_index);
    const bool tri = prim == AmdGpu::PrimitiveType::TriangleList;
    const auto minus = m.Constant(f32, -1.0f), plus = m.Constant(f32, tri ? 3.0f : 1.0f);
    const auto is = [&](s32 v) { return m.OpIEqual(b, idx, m.Constant(int_t, v)); };
    Sirit::Id x_far = is(1), y_far = is(2);
    if (prim == AmdGpu::PrimitiveType::QuadList) {
        x_far = m.OpLogicalOr(b, is(1), is(2));
        y_far = m.OpLogicalOr(b, is(2), is(3));
    }
    const auto x = m.OpSelect(f32, x_far, plus, minus);
    const auto y = m.OpSelect(f32, y_far, plus, minus);
    m.OpStore(position, m.OpCompositeConstruct(v4, x, y, m.Constant(f32, 0.0f),
                                               m.Constant(f32, 1.0f)));
    for (const auto& [loc, out] : outputs) {
        m.OpStore(out, m.ConstantComposite(v4, m.Constant(f32, VsValue(loc, 0)),
                                           m.Constant(f32, VsValue(loc, 1)),
                                           m.Constant(f32, VsValue(loc, 2)),
                                           m.Constant(f32, VsValue(loc, 3))));
    }
    m.OpReturn();
    m.OpFunctionEnd();
    m.AddEntryPoint(spv::ExecutionModel::Vertex, fn, "main", interfaces);
    m.SetMemoryModel(spv::AddressingModel::Logical, spv::MemoryModel::GLSL450);
    return m.Assemble();
}

// Fragment shader: one colour output per read location (up to 4), each the raw input vec4.
std::vector<u32> FragmentShader(const std::vector<u32>& read_locations) {
    Sirit::Module m(0x00010500);
    m.AddCapability(spv::Capability::Shader);
    const auto f32 = m.TypeFloat(32), v4 = m.TypeVector(f32, 4);
    std::vector<Sirit::Id> interfaces;
    std::vector<std::pair<Sirit::Id, Sirit::Id>> io;
    for (u32 i = 0; i < read_locations.size(); ++i) {
        const auto in = m.AddGlobalVariable(m.TypePointer(spv::StorageClass::Input, v4),
                                            spv::StorageClass::Input);
        m.Decorate(in, spv::Decoration::Location, read_locations[i]);
        const auto out = m.AddGlobalVariable(m.TypePointer(spv::StorageClass::Output, v4),
                                             spv::StorageClass::Output);
        m.Decorate(out, spv::Decoration::Location, i);
        interfaces.push_back(in);
        interfaces.push_back(out);
        io.emplace_back(in, out);
    }
    const auto fn = m.OpFunction(m.TypeVoid(), spv::FunctionControlMask::MaskNone,
                                 m.TypeFunction(m.TypeVoid()));
    m.AddLabel(m.OpLabel());
    for (const auto& [in, out] : io) {
        m.OpStore(out, m.OpLoad(v4, in));
    }
    m.OpReturn();
    m.OpFunctionEnd();
    m.AddEntryPoint(spv::ExecutionModel::Fragment, fn, "main", interfaces);
    m.AddExecutionMode(fn, spv::ExecutionMode::OriginUpperLeft);
    m.SetMemoryModel(spv::AddressingModel::Logical, spv::MemoryModel::GLSL450);
    return m.Assemble();
}

struct Gpu {
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    VkQueue queue;
    u32 family;
    VkCommandPool pool;
    VkPhysicalDeviceMemoryProperties mem;
};

u32 MemoryType(const Gpu& g, u32 bits, VkMemoryPropertyFlags want) {
    for (u32 i = 0; i < g.mem.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) && (g.mem.memoryTypes[i].propertyFlags & want) == want) {
            return i;
        }
    }
    std::fprintf(stderr, "FATAL: no memory type\n");
    std::exit(2);
}

Gpu MakeGpu() {
    Gpu g{};
    const VkApplicationInfo app{.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                .pApplicationName = "rect interface",
                                .apiVersion = VK_API_VERSION_1_3};
    const char* ext = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
    const VkInstanceCreateInfo ici{.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                   .flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR,
                                   .pApplicationInfo = &app,
                                   .enabledExtensionCount = 1,
                                   .ppEnabledExtensionNames = &ext};
    CHECK(vkCreateInstance(&ici, nullptr, &g.instance));
    u32 count = 1;
    vkEnumeratePhysicalDevices(g.instance, &count, &g.physical);
    if (count == 0) {
        std::fprintf(stderr, "FATAL: no device\n");
        std::exit(2);
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(g.physical, &props);
    std::fprintf(stderr, "device: %s api %u.%u.%u\n", props.deviceName,
                 VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
                 VK_API_VERSION_PATCH(props.apiVersion));
    vkGetPhysicalDeviceMemoryProperties(g.physical, &g.mem);
    u32 nfam = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g.physical, &nfam, nullptr);
    std::vector<VkQueueFamilyProperties> fams(nfam);
    vkGetPhysicalDeviceQueueFamilyProperties(g.physical, &nfam, fams.data());
    while (!(fams.at(g.family).queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
        ++g.family;
    }
    const float prio = 1.0f;
    const VkDeviceQueueCreateInfo qci{.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                      .queueFamilyIndex = g.family,
                                      .queueCount = 1,
                                      .pQueuePriorities = &prio};
    VkPhysicalDeviceVulkan13Features f13{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                         .dynamicRendering = VK_TRUE};
    const VkPhysicalDeviceFeatures features{.tessellationShader = VK_TRUE};
    const char* dext = "VK_KHR_portability_subset";
    const VkDeviceCreateInfo dci{.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                 .pNext = &f13,
                                 .queueCreateInfoCount = 1,
                                 .pQueueCreateInfos = &qci,
                                 .enabledExtensionCount = 1,
                                 .ppEnabledExtensionNames = &dext,
                                 .pEnabledFeatures = &features};
    CHECK(vkCreateDevice(g.physical, &dci, nullptr, &g.device));
    vkGetDeviceQueue(g.device, g.family, 0, &g.queue);
    const VkCommandPoolCreateInfo pci{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                      .queueFamilyIndex = g.family};
    CHECK(vkCreateCommandPool(g.device, &pci, nullptr, &g.pool));
    return g;
}

VkShaderModule Module(const Gpu& g, const std::vector<u32>& code) {
    const VkShaderModuleCreateInfo ci{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                      .codeSize = code.size() * 4,
                                      .pCode = code.data()};
    VkShaderModule m;
    CHECK(vkCreateShaderModule(g.device, &ci, nullptr, &m));
    return m;
}

struct Case {
    const char* name;
    AmdGpu::PrimitiveType prim; // RectList, QuadList (aux tess) or TriangleList (direct)
    u32 vs_written;             // VS output location mask
    std::vector<u32> fs_reads;  // FS input locations, in FS input order
    bool motion = false;        // fs_info.motion_vectors: locations 30/31 come from the flag
    // Metal refuses to link the pipeline. Production names the gap before reaching the driver
    // (vk_graphics_pipeline.cpp, Apple hard error); this case pins the driver behaviour that
    // makes that check necessary.
    bool expect_link_failure = false;
};


struct Result {
    bool created;
    std::vector<std::array<float, 4>> centre; // one per FS output
    u32 covered;                              // pixels of output 0 not equal to Clear
};

Result Run(const Gpu& g, const Case& c) {
    Result res{};
    const bool aux = c.prim != AmdGpu::PrimitiveType::TriangleList;
    const bool quad = c.prim == AmdGpu::PrimitiveType::QuadList;
    const u32 n_out = u32(c.fs_reads.size());

    // Colour targets + readback buffers.
    std::vector<VkImage> images(n_out);
    std::vector<VkImageView> views(n_out);
    std::vector<VkDeviceMemory> image_mem(n_out);
    const VkDeviceSize bytes = Size * Size * 16;
    VkBuffer readback;
    VkDeviceMemory readback_mem;
    {
        const VkBufferCreateInfo bci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                     .size = bytes * n_out,
                                     .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        CHECK(vkCreateBuffer(g.device, &bci, nullptr, &readback));
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(g.device, readback, &req);
        const VkMemoryAllocateInfo mai{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = req.size,
            .memoryTypeIndex = MemoryType(g, req.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
        CHECK(vkAllocateMemory(g.device, &mai, nullptr, &readback_mem));
        CHECK(vkBindBufferMemory(g.device, readback, readback_mem, 0));
    }
    for (u32 i = 0; i < n_out; ++i) {
        const VkImageCreateInfo ici{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                    .imageType = VK_IMAGE_TYPE_2D,
                                    .format = VK_FORMAT_R32G32B32A32_SFLOAT,
                                    .extent = {Size, Size, 1},
                                    .mipLevels = 1,
                                    .arrayLayers = 1,
                                    .samples = VK_SAMPLE_COUNT_1_BIT,
                                    .tiling = VK_IMAGE_TILING_OPTIMAL,
                                    .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                             VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
        CHECK(vkCreateImage(g.device, &ici, nullptr, &images[i]));
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(g.device, images[i], &req);
        const VkMemoryAllocateInfo mai{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = req.size,
            .memoryTypeIndex =
                MemoryType(g, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
        CHECK(vkAllocateMemory(g.device, &mai, nullptr, &image_mem[i]));
        CHECK(vkBindImageMemory(g.device, images[i], image_mem[i], 0));
        const VkImageViewCreateInfo vci{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = images[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = VK_FORMAT_R32G32B32A32_SFLOAT,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        CHECK(vkCreateImageView(g.device, &vci, nullptr, &views[i]));
    }

    // Shaders: real auxiliary stages from the production emitter.
    Shader::HwFragmentRuntimeInfo fs_info{};
    fs_info.motion_vectors = c.motion;
    for (const u32 loc : c.fs_reads) {
        if (c.motion && (loc == Shader::MotionVectors::CurrentLocation ||
                         loc == Shader::MotionVectors::PreviousLocation)) {
            continue;
        }
        fs_info.inputs[fs_info.num_inputs++] = {
            .param_index = u8(loc), .is_default = false, .is_flat = false, .default_value = 0};
    }

    std::vector<VkShaderModule> modules;
    std::vector<VkPipelineShaderStageCreateInfo> stages;
    auto add = [&](VkShaderStageFlagBits stage, const std::vector<u32>& code) {
        modules.push_back(Module(g, code));
        stages.push_back({.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                          .stage = stage,
                          .module = modules.back(),
                          .pName = "main"});
    };
    add(VK_SHADER_STAGE_VERTEX_BIT, VertexShader(c.vs_written, c.prim));
    if (aux) {
        const auto tcs = quad ? AuxShaderType::QuadListTCS : AuxShaderType::RectListTCS;
        add(VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
            EmitAuxilaryTessShader(tcs, fs_info, c.vs_written));
        add(VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,
            EmitAuxilaryTessShader(AuxShaderType::PassthroughTES, fs_info, c.vs_written));
    }
    add(VK_SHADER_STAGE_FRAGMENT_BIT, FragmentShader(c.fs_reads));

    const VkPipelineLayoutCreateInfo lci{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkPipelineLayout layout;
    CHECK(vkCreatePipelineLayout(g.device, &lci, nullptr, &layout));
    const VkPipelineVertexInputStateCreateInfo vi{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    const VkPipelineInputAssemblyStateCreateInfo ia{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = aux ? VK_PRIMITIVE_TOPOLOGY_PATCH_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    const VkPipelineTessellationStateCreateInfo ts{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO,
        .patchControlPoints = quad ? 4u : 3u};
    const VkViewport viewport{0, 0, float(Size), float(Size), 0, 1};
    const VkRect2D scissor{{0, 0}, {Size, Size}};
    const VkPipelineViewportStateCreateInfo vps{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .pViewports = &viewport,
        .scissorCount = 1,
        .pScissors = &scissor};
    const VkPipelineRasterizationStateCreateInfo rs{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .lineWidth = 1.0f};
    const VkPipelineMultisampleStateCreateInfo ms{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    std::vector<VkPipelineColorBlendAttachmentState> blends(
        n_out, VkPipelineColorBlendAttachmentState{.colorWriteMask = 0xF});
    const VkPipelineColorBlendStateCreateInfo cb{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = n_out,
        .pAttachments = blends.data()};
    std::vector<VkFormat> formats(n_out, VK_FORMAT_R32G32B32A32_SFLOAT);
    const VkPipelineRenderingCreateInfo rci{.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                            .colorAttachmentCount = n_out,
                                            .pColorAttachmentFormats = formats.data()};
    const VkGraphicsPipelineCreateInfo gci{.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                           .pNext = &rci,
                                           .stageCount = u32(stages.size()),
                                           .pStages = stages.data(),
                                           .pVertexInputState = &vi,
                                           .pInputAssemblyState = &ia,
                                           .pTessellationState = aux ? &ts : nullptr,
                                           .pViewportState = &vps,
                                           .pRasterizationState = &rs,
                                           .pMultisampleState = &ms,
                                           .pColorBlendState = &cb,
                                           .layout = layout};
    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult pr = vkCreateGraphicsPipelines(g.device, VK_NULL_HANDLE, 1, &gci, nullptr,
                                                  &pipeline);
    res.created = pr == VK_SUCCESS && pipeline != VK_NULL_HANDLE;
    if (!res.created) {
        std::fprintf(stderr, "  pipeline creation -> %d\n", int(pr));
    }

    VkCommandBuffer cmd;
    const VkCommandBufferAllocateInfo cai{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                          .commandPool = g.pool,
                                          .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                          .commandBufferCount = 1};
    CHECK(vkAllocateCommandBuffers(g.device, &cai, &cmd));
    const VkCommandBufferBeginInfo bi{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    CHECK(vkBeginCommandBuffer(cmd, &bi));
    std::vector<VkImageMemoryBarrier> to_color, to_src;
    for (u32 i = 0; i < n_out; ++i) {
        to_color.push_back({.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                            .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                            .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                            .image = images[i],
                            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}});
        to_src.push_back({.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                          .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                          .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                          .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                          .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                          .image = images[i],
                          .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}});
    }
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr,
                         n_out, to_color.data());
    std::vector<VkRenderingAttachmentInfo> atts;
    for (u32 i = 0; i < n_out; ++i) {
        VkClearValue clear{};
        clear.color = {{Clear, Clear, Clear, Clear}};
        atts.push_back({.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                        .imageView = views[i],
                        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                        .clearValue = clear});
    }
    const VkRenderingInfo ri{.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                             .renderArea = scissor,
                             .layerCount = 1,
                             .colorAttachmentCount = n_out,
                             .pColorAttachments = atts.data()};
    vkCmdBeginRendering(cmd, &ri);
    if (res.created) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdDraw(cmd, quad ? 4 : 3, 1, 0, 0);
    }
    vkCmdEndRendering(cmd);
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, n_out,
                         to_src.data());
    for (u32 i = 0; i < n_out; ++i) {
        const VkBufferImageCopy copy{.bufferOffset = bytes * i,
                                     .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                     .imageExtent = {Size, Size, 1}};
        vkCmdCopyImageToBuffer(cmd, images[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1,
                               &copy);
    }
    CHECK(vkEndCommandBuffer(cmd));
    const VkSubmitInfo si{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                          .commandBufferCount = 1,
                          .pCommandBuffers = &cmd};
    CHECK(vkQueueSubmit(g.queue, 1, &si, VK_NULL_HANDLE));
    CHECK(vkQueueWaitIdle(g.queue));

    float* px;
    CHECK(vkMapMemory(g.device, readback_mem, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(&px)));
    for (u32 i = 0; i < n_out; ++i) {
        const float* img = px + (bytes / 4) * i;
        const float* centre = img + ((Size / 2) * Size + Size / 2) * 4;
        res.centre.push_back({centre[0], centre[1], centre[2], centre[3]});
        if (i == 0) {
            for (u32 p = 0; p < Size * Size; ++p) {
                res.covered += img[p * 4] != Clear ? 1 : 0;
            }
        }
    }
    vkUnmapMemory(g.device, readback_mem);

    vkDeviceWaitIdle(g.device);
    vkFreeCommandBuffers(g.device, g.pool, 1, &cmd);
    if (pipeline) {
        vkDestroyPipeline(g.device, pipeline, nullptr);
    }
    vkDestroyPipelineLayout(g.device, layout, nullptr);
    for (auto m : modules) {
        vkDestroyShaderModule(g.device, m, nullptr);
    }
    for (u32 i = 0; i < n_out; ++i) {
        vkDestroyImageView(g.device, views[i], nullptr);
        vkDestroyImage(g.device, images[i], nullptr);
        vkFreeMemory(g.device, image_mem[i], nullptr);
    }
    vkDestroyBuffer(g.device, readback, nullptr);
    vkFreeMemory(g.device, readback_mem, nullptr);
    return res;
}

} // namespace

int main() {
    using P = AmdGpu::PrimitiveType;
    const Gpu g = MakeGpu();
    const std::vector<Case> cases = {
        {"rect: FS reads exactly the VS outputs", P::RectList, 0b1, {0}},
        {"rect: VS writes extra locations FS ignores", P::RectList, 0b1011, {1, 3}},
        {"rect: FS reads location VS never wrote", P::RectList, 0b1, {0, 1}},
        {"rect: FS reads only unwritten locations", P::RectList, 0b100, {0, 5}},
        {"rect: VS writes nothing, FS reads two", P::RectList, 0b0, {2, 7}},
        {"quad: FS reads exactly the VS outputs", P::QuadList, 0b110, {1, 2}},
        {"quad: FS reads location VS never wrote", P::QuadList, 0b10, {1, 4}},
        {"quad: motion locations 30/31 forwarded", P::QuadList, 0b11u << 30 | 0b1, {0, 30, 31},
         true},
        {"quad: motion read, VS motion off", P::QuadList, 0b1, {0, 30, 31}, true},
        {"direct: FS reads exactly the VS outputs", P::TriangleList, 0b1, {0}},
        {"direct: FS reads location VS never wrote (Metal refuses)", P::TriangleList, 0b1, {0, 1},
         false, true},
    };
#ifdef BB_RECT_IFACE_BASELINE // the pre-fix emitter has no mask helpers; render cases only
    u32 failures = 0;
#else
    u32 failures = CheckMasks();
#endif
    for (const auto& c : cases) {
        std::fprintf(stderr, "%s\n", c.name);
        const Result r = Run(g, c);
        if (c.expect_link_failure) {
            const bool ok = !r.created;
            std::fprintf(stderr, "  created=%d (expected refusal) -> %s\n", r.created,
                         ok ? "PASS" : "FAIL");
            failures += ok ? 0 : 1;
            continue;
        }
        bool ok = r.created && r.covered == Size * Size;
        for (u32 i = 0; i < c.fs_reads.size() && r.created; ++i) {
            const u32 loc = c.fs_reads[i];
            const bool written = c.vs_written & (1u << loc);
            const auto& v = r.centre[i];
            bool match = true;
            for (u32 comp = 0; comp < 4; ++comp) {
                const float want = written ? VsValue(loc, comp) : 0.0f;
                match &= v[comp] == want;
            }
            std::fprintf(stderr, "  loc %2u %-9s -> (%g, %g, %g, %g) %s\n", loc,
                         written ? "written" : "unwritten", v[0], v[1], v[2], v[3],
                         match ? "ok" : "MISMATCH");
            ok &= match;
        }
        std::fprintf(stderr, "  created=%d covered=%u/%u -> %s\n", r.created, r.covered,
                     Size * Size, ok ? "PASS" : "FAIL");
        failures += ok ? 0 : 1;
    }
#ifdef BB_RECT_IFACE_BASELINE
    constexpr const char* mask_checks = "";
#else
    constexpr const char* mask_checks = " + 7 mask checks";
#endif
    std::fprintf(stderr, "%s: %u failures (%zu render cases%s)\n", failures ? "FAIL" : "PASS",
                 failures, cases.size(), mask_checks);
    return failures ? 1 : 0;
}
