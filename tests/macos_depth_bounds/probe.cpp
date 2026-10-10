// SPDX-License-Identifier: GPL-2.0-or-later
// MoltenVK feasibility probe for depth-bounds emulation: can a draw sample the depth attachment it
// is rendering with, in a read-only depth layout (no copy, no render-pass break)?
//
// Pass 1 stores depth 0.25 (left half, clear) / 0.75 (right half, drawn). Pass 2 keeps the same
// image attached (read-only depth, with stencil either read-only or writable), samples its depth
// aspect at the fragment's pixel and discards outside [0.5, 1]; the colour target records the
// fetched depth. Expected: left discarded (clear value), right 0.75. A second variant writes stencil
// in pass 2 (DEPTH_READ_ONLY_STENCIL_ATTACHMENT layout) and checks the stencil result too.
#include <vulkan/vulkan.h>
#include <sirit/sirit.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "common/types.h"

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
constexpr VkFormat DepthFormat = VK_FORMAT_D32_SFLOAT_S8_UINT;

VkDevice dev;
VkPhysicalDevice phys;
VkQueue queue;
VkCommandPool pool;
VkPhysicalDeviceMemoryProperties memprops;

u32 MemType(u32 bits, VkMemoryPropertyFlags want) {
    for (u32 i = 0; i < memprops.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (memprops.memoryTypes[i].propertyFlags & want) == want) return i;
    std::exit(2);
}

// Fullscreen-ish quad (two triangles) covering x in [x0, x1], at depth z (push constants).
std::vector<u32> VertexShader() {
    Sirit::Module m(0x00010500);
    m.AddCapability(spv::Capability::Shader);
    const auto f = m.TypeFloat(32), i32 = m.TypeInt(32, true), b = m.TypeBool();
    const auto v4 = m.TypeVector(f, 4);
    const auto pc_t = m.TypeStruct(f, f, f);
    m.Decorate(pc_t, spv::Decoration::Block);
    m.MemberDecorate(pc_t, 0, spv::Decoration::Offset, 0u);
    m.MemberDecorate(pc_t, 1, spv::Decoration::Offset, 4u);
    m.MemberDecorate(pc_t, 2, spv::Decoration::Offset, 8u);
    const auto pc = m.AddGlobalVariable(m.TypePointer(spv::StorageClass::PushConstant, pc_t),
                                        spv::StorageClass::PushConstant);
    const auto pos = m.AddGlobalVariable(m.TypePointer(spv::StorageClass::Output, v4),
                                         spv::StorageClass::Output);
    m.Decorate(pos, spv::Decoration::BuiltIn, spv::BuiltIn::Position);
    const auto vid = m.AddGlobalVariable(m.TypePointer(spv::StorageClass::Input, i32),
                                         spv::StorageClass::Input);
    m.Decorate(vid, spv::Decoration::BuiltIn, spv::BuiltIn::VertexIndex);
    const auto fn = m.OpFunction(m.TypeVoid(), spv::FunctionControlMask::MaskNone,
                                 m.TypeFunction(m.TypeVoid()));
    m.AddLabel(m.OpLabel());
    const auto pf = m.TypePointer(spv::StorageClass::PushConstant, f);
    const auto load = [&](u32 i) {
        return m.OpLoad(f, m.OpAccessChain(pf, pc, m.Constant(m.TypeInt(32, false), i)));
    };
    const auto x0 = load(0), x1 = load(1), z = load(2);
    const auto idx = m.OpLoad(i32, vid);
    const auto is = [&](s32 v) { return m.OpIEqual(b, idx, m.Constant(i32, v)); };
    // 0:(x0,-1) 1:(x1,-1) 2:(x0,1) 3:(x1,-1) 4:(x1,1) 5:(x0,1)
    const auto right = m.OpLogicalOr(b, m.OpLogicalOr(b, is(1), is(3)), is(4));
    const auto top = m.OpLogicalOr(b, m.OpLogicalOr(b, is(2), is(4)), is(5));
    const auto x = m.OpSelect(f, right, x1, x0);
    const auto y = m.OpSelect(f, top, m.Constant(f, 1.0f), m.Constant(f, -1.0f));
    m.OpStore(pos, m.OpCompositeConstruct(v4, x, y, z, m.Constant(f, 1.0f)));
    m.OpReturn();
    m.OpFunctionEnd();
    const std::array ifaces{pc, pos, vid};
    m.AddEntryPoint(spv::ExecutionModel::Vertex, fn, "main", ifaces);
    m.SetMemoryModel(spv::AddressingModel::Logical, spv::MemoryModel::GLSL450);
    return m.Assemble();
}

// Fetches the stored depth at the pixel; kills outside [0.5, 1]; writes it to colour 0.
std::vector<u32> BoundsShader() {
    Sirit::Module m(0x00010500);
    m.AddCapability(spv::Capability::Shader);
    const auto f = m.TypeFloat(32), i32 = m.TypeInt(32, true), b = m.TypeBool();
    const auto v4 = m.TypeVector(f, 4), iv2 = m.TypeVector(i32, 2);
    const auto img_t = m.TypeImage(f, spv::Dim::Dim2D, 0, false, false, 1, spv::ImageFormat::Unknown);
    const auto img = m.AddGlobalVariable(m.TypePointer(spv::StorageClass::UniformConstant, img_t),
                                         spv::StorageClass::UniformConstant);
    m.Decorate(img, spv::Decoration::DescriptorSet, 0u);
    m.Decorate(img, spv::Decoration::Binding, 0u);
    const auto coord = m.AddGlobalVariable(m.TypePointer(spv::StorageClass::Input, v4),
                                           spv::StorageClass::Input);
    m.Decorate(coord, spv::Decoration::BuiltIn, spv::BuiltIn::FragCoord);
    const auto out = m.AddGlobalVariable(m.TypePointer(spv::StorageClass::Output, v4),
                                         spv::StorageClass::Output);
    m.Decorate(out, spv::Decoration::Location, 0u);
    const auto fn = m.OpFunction(m.TypeVoid(), spv::FunctionControlMask::MaskNone,
                                 m.TypeFunction(m.TypeVoid()));
    m.AddLabel(m.OpLabel());
    const auto fc = m.OpLoad(v4, coord);
    const auto xy = m.OpConvertFToS(iv2, m.OpVectorShuffle(m.TypeVector(f, 2), fc, fc, 0, 1));
    const auto texel = m.OpImageFetch(v4, m.OpLoad(img_t, img), xy, spv::ImageOperandsMask::Lod,
                                      m.Constant(i32, 0));
    const auto d = m.OpCompositeExtract(f, texel, 0);
    const auto inside = m.OpLogicalAnd(b, m.OpFOrdLessThanEqual(b, m.Constant(f, 0.5f), d),
                                       m.OpFOrdLessThanEqual(b, d, m.Constant(f, 1.0f)));
    const auto keep = m.OpLabel(), kill = m.OpLabel();
    m.OpSelectionMerge(keep, spv::SelectionControlMask::MaskNone);
    m.OpBranchConditional(inside, keep, kill);
    m.AddLabel(kill);
    m.OpKill();
    m.AddLabel(keep);
    m.OpStore(out, m.OpCompositeConstruct(v4, d, d, d, m.Constant(f, 1.0f)));
    m.OpReturn();
    m.OpFunctionEnd();
    const std::array ifaces{img, coord, out};
    m.AddEntryPoint(spv::ExecutionModel::Fragment, fn, "main", ifaces);
    m.AddExecutionMode(fn, spv::ExecutionMode::OriginUpperLeft);
    m.SetMemoryModel(spv::AddressingModel::Logical, spv::MemoryModel::GLSL450);
    return m.Assemble();
}

VkShaderModule Module(const std::vector<u32>& code) {
    VkShaderModuleCreateInfo ci{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                .codeSize = code.size() * 4, .pCode = code.data()};
    VkShaderModule mod;
    CHECK(vkCreateShaderModule(dev, &ci, nullptr, &mod));
    return mod;
}

void Barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
             VkImageAspectFlags aspect) {
    VkImageMemoryBarrier b{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                           .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                           .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                           .oldLayout = from, .newLayout = to,
                           .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                           .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = image,
                           .subresourceRange = {aspect, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &b);
}

struct Result {
    std::array<float, 2> colour; // left, right centre pixels
    std::array<u8, 2> stencil;
};

Result Run(bool stencil_writable) {
    // Images: depth (D32S8, attachment + sampled), colour (R32F), readback buffer.
    auto make_image = [&](VkFormat format, VkImageUsageFlags usage, VkImage& image,
                          VkDeviceMemory& mem) {
        VkImageCreateInfo ci{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                             .imageType = VK_IMAGE_TYPE_2D, .format = format,
                             .extent = {Size, Size, 1}, .mipLevels = 1, .arrayLayers = 1,
                             .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                             .usage = usage};
        CHECK(vkCreateImage(dev, &ci, nullptr, &image));
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(dev, image, &req);
        VkMemoryAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                .allocationSize = req.size,
                                .memoryTypeIndex = MemType(req.memoryTypeBits,
                                                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
        CHECK(vkAllocateMemory(dev, &ai, nullptr, &mem));
        CHECK(vkBindImageMemory(dev, image, mem, 0));
    };
    VkImage depth, colour;
    VkDeviceMemory depth_mem, colour_mem;
    make_image(DepthFormat,
               VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                   VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
               depth, depth_mem);
    make_image(VK_FORMAT_R32_SFLOAT,
               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, colour,
               colour_mem);
    auto view = [&](VkImage image, VkFormat format, VkImageAspectFlags aspect) {
        VkImageViewCreateInfo ci{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = image,
                                 .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = format,
                                 .subresourceRange = {aspect, 0, 1, 0, 1}};
        VkImageView v;
        CHECK(vkCreateImageView(dev, &ci, nullptr, &v));
        return v;
    };
    const VkImageView ds_view =
        view(depth, DepthFormat, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
    const VkImageView depth_sample_view = view(depth, DepthFormat, VK_IMAGE_ASPECT_DEPTH_BIT);
    const VkImageView colour_view = view(colour, VK_FORMAT_R32_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT);

    VkBuffer rb;
    VkDeviceMemory rb_mem;
    {
        VkBufferCreateInfo ci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                              .size = Size * Size * 4 * 2, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        CHECK(vkCreateBuffer(dev, &ci, nullptr, &rb));
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(dev, rb, &req);
        VkMemoryAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                .allocationSize = req.size,
                                .memoryTypeIndex = MemType(req.memoryTypeBits,
                                                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
        CHECK(vkAllocateMemory(dev, &ai, nullptr, &rb_mem));
        CHECK(vkBindBufferMemory(dev, rb, rb_mem, 0));
    }

    // Descriptor: the depth aspect as a sampled image.
    VkDescriptorSetLayoutBinding binding{.binding = 0,
                                         .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                         .descriptorCount = 1,
                                         .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT};
    VkDescriptorSetLayoutCreateInfo dslci{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                          .bindingCount = 1, .pBindings = &binding};
    VkDescriptorSetLayout dsl;
    CHECK(vkCreateDescriptorSetLayout(dev, &dslci, nullptr, &dsl));
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1};
    VkDescriptorPoolCreateInfo dpci{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                    .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps};
    VkDescriptorPool dpool;
    CHECK(vkCreateDescriptorPool(dev, &dpci, nullptr, &dpool));
    VkDescriptorSetAllocateInfo dsai{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                     .descriptorPool = dpool, .descriptorSetCount = 1,
                                     .pSetLayouts = &dsl};
    VkDescriptorSet set;
    CHECK(vkAllocateDescriptorSets(dev, &dsai, &set));
    const VkImageLayout read_layout = stencil_writable
                                          ? VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL
                                          : VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo dii{.imageView = depth_sample_view, .imageLayout = read_layout};
    VkWriteDescriptorSet w{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
                           .descriptorCount = 1,
                           .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = &dii};
    vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);

    VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT, 0, 12};
    VkPipelineLayoutCreateInfo plci{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                    .setLayoutCount = 1, .pSetLayouts = &dsl,
                                    .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr};
    VkPipelineLayout layout;
    CHECK(vkCreatePipelineLayout(dev, &plci, nullptr, &layout));

    const VkShaderModule vs = Module(VertexShader()), fs = Module(BoundsShader());
    auto pipeline = [&](bool with_fs, bool depth_write, bool stencil_write) {
        VkPipelineShaderStageCreateInfo stages[2] = {
            {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
             .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main"},
            {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
             .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main"}};
        VkPipelineVertexInputStateCreateInfo vi{.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                  .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
        VkViewport vp{0, 0, float(Size), float(Size), 0, 1};
        VkRect2D sc{{0, 0}, {Size, Size}};
        VkPipelineViewportStateCreateInfo vps{.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                              .viewportCount = 1, .pViewports = &vp,
                                              .scissorCount = 1, .pScissors = &sc};
        VkPipelineRasterizationStateCreateInfo rs{.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                  .polygonMode = VK_POLYGON_MODE_FILL,
                                                  .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1};
        VkPipelineMultisampleStateCreateInfo ms{.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                                .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
        VkStencilOpState st{.failOp = VK_STENCIL_OP_KEEP, .passOp = VK_STENCIL_OP_REPLACE,
                            .depthFailOp = VK_STENCIL_OP_KEEP, .compareOp = VK_COMPARE_OP_ALWAYS,
                            .compareMask = 0xFF, .writeMask = 0xFF, .reference = 0x5A};
        VkPipelineDepthStencilStateCreateInfo ds{.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                                                 .depthTestEnable = VK_TRUE,
                                                 .depthWriteEnable = depth_write,
                                                 .depthCompareOp = VK_COMPARE_OP_ALWAYS,
                                                 .stencilTestEnable = stencil_write,
                                                 .front = st, .back = st};
        VkPipelineColorBlendAttachmentState cba{.colorWriteMask = 0xF};
        VkPipelineColorBlendStateCreateInfo cb{.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                               .attachmentCount = with_fs ? 1u : 0u,
                                               .pAttachments = &cba};
        const VkFormat cf = VK_FORMAT_R32_SFLOAT;
        VkPipelineRenderingCreateInfo rci{.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                          .colorAttachmentCount = with_fs ? 1u : 0u,
                                          .pColorAttachmentFormats = &cf,
                                          .depthAttachmentFormat = DepthFormat,
                                          .stencilAttachmentFormat = DepthFormat};
        VkGraphicsPipelineCreateInfo gci{.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                         .pNext = &rci, .stageCount = with_fs ? 2u : 1u,
                                         .pStages = stages, .pVertexInputState = &vi,
                                         .pInputAssemblyState = &ia, .pViewportState = &vps,
                                         .pRasterizationState = &rs, .pMultisampleState = &ms,
                                         .pDepthStencilState = &ds, .pColorBlendState = &cb,
                                         .layout = layout};
        VkPipeline p{};
        const VkResult r = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gci, nullptr, &p);
        if (r != VK_SUCCESS) std::fprintf(stderr, "  pipeline creation -> %d\n", int(r));
        return p;
    };
    const VkPipeline write_depth = pipeline(false, true, false);
    const VkPipeline bounds = pipeline(true, false, stencil_writable);

    VkCommandBuffer cmd;
    VkCommandBufferAllocateInfo cai{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                    .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                    .commandBufferCount = 1};
    CHECK(vkAllocateCommandBuffers(dev, &cai, &cmd));
    VkCommandBufferBeginInfo bi{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CHECK(vkBeginCommandBuffer(cmd, &bi));
    const VkImageAspectFlags ds_aspects = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    Barrier(cmd, depth, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            ds_aspects);
    Barrier(cmd, colour, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_ASPECT_COLOR_BIT);
    // Pass 1: depth 0.25 everywhere (clear), stencil 0x11, then 0.75 on the right half.
    {
        VkRenderingAttachmentInfo da{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                     .imageView = ds_view,
                                     .imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                     .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                     .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
        da.clearValue.depthStencil = {0.25f, 0x11};
        VkRenderingInfo ri{.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {Size, Size}},
                           .layerCount = 1, .pDepthAttachment = &da, .pStencilAttachment = &da};
        vkCmdBeginRendering(cmd, &ri);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, write_depth);
        const float pc[3] = {0.0f, 1.0f, 0.75f};
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 12, pc);
        vkCmdDraw(cmd, 6, 1, 0, 0);
        vkCmdEndRendering(cmd);
    }
    Barrier(cmd, depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, read_layout, ds_aspects);
    // Pass 2: same depth image attached read-only, sampled by the bounds shader.
    {
        VkRenderingAttachmentInfo ca{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                     .imageView = colour_view,
                                     .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                     .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                     .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
        ca.clearValue.color = {{-1.0f, 0, 0, 0}};
        VkRenderingAttachmentInfo da{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                     .imageView = ds_view, .imageLayout = read_layout,
                                     .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                     .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
        VkRenderingInfo ri{.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {Size, Size}},
                           .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &ca,
                           .pDepthAttachment = &da, .pStencilAttachment = &da};
        vkCmdBeginRendering(cmd, &ri);
        if (bounds) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bounds);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0,
                                    nullptr);
            const float pc[3] = {-1.0f, 1.0f, 0.5f};
            vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 12, pc);
            vkCmdDraw(cmd, 6, 1, 0, 0);
        }
        vkCmdEndRendering(cmd);
    }
    Barrier(cmd, colour, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    Barrier(cmd, depth, read_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, ds_aspects);
    VkBufferImageCopy c0{.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                         .imageExtent = {Size, Size, 1}};
    vkCmdCopyImageToBuffer(cmd, colour, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb, 1, &c0);
    VkBufferImageCopy c1{.bufferOffset = Size * Size * 4,
                         .imageSubresource = {VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 1},
                         .imageExtent = {Size, Size, 1}};
    vkCmdCopyImageToBuffer(cmd, depth, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb, 1, &c1);
    CHECK(vkEndCommandBuffer(cmd));
    VkSubmitInfo si{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1,
                    .pCommandBuffers = &cmd};
    CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
    CHECK(vkQueueWaitIdle(queue));
    void* p;
    CHECK(vkMapMemory(dev, rb_mem, 0, VK_WHOLE_SIZE, 0, &p));
    const float* col = static_cast<const float*>(p);
    const u8* sten = static_cast<const u8*>(p) + Size * Size * 4;
    const u32 row = Size / 2, left = Size / 4, right = 3 * Size / 4;
    Result r{{col[row * Size + left], col[row * Size + right]},
             {sten[row * Size + left], sten[row * Size + right]}};
    vkUnmapMemory(dev, rb_mem);
    return r;
}
} // namespace

int main() {
    const char* ext = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
    VkApplicationInfo app{.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici{.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                             .flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR,
                             .pApplicationInfo = &app, .enabledExtensionCount = 1,
                             .ppEnabledExtensionNames = &ext};
    VkInstance inst;
    CHECK(vkCreateInstance(&ici, nullptr, &inst));
    u32 n = 1;
    vkEnumeratePhysicalDevices(inst, &n, &phys);
    vkGetPhysicalDeviceMemoryProperties(phys, &memprops);
    const float prio = 1;
    VkDeviceQueueCreateInfo qci{.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &prio};
    VkPhysicalDeviceVulkan13Features f13{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                         .dynamicRendering = VK_TRUE};
    const char* dext = "VK_KHR_portability_subset";
    VkDeviceCreateInfo dci{.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f13,
                           .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
                           .enabledExtensionCount = 1, .ppEnabledExtensionNames = &dext};
    CHECK(vkCreateDevice(phys, &dci, nullptr, &dev));
    vkGetDeviceQueue(dev, 0, 0, &queue);
    VkCommandPoolCreateInfo pci{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                .queueFamilyIndex = 0};
    CHECK(vkCreateCommandPool(dev, &pci, nullptr, &pool));

    u32 failures = 0;
    for (const bool stencil_writable : {false, true}) {
        const Result r = Run(stencil_writable);
        const bool colour_ok = r.colour[0] == -1.0f && r.colour[1] == 0.75f;
        const bool stencil_ok = !stencil_writable || (r.stencil[0] == 0x11 && r.stencil[1] == 0x5A);
        std::fprintf(stderr,
                     "%s: left %g (want -1, killed), right %g (want 0.75); stencil left %#x right "
                     "%#x%s -> %s\n",
                     stencil_writable ? "depth read-only, stencil written"
                                      : "depth+stencil read-only",
                     r.colour[0], r.colour[1], r.stencil[0], r.stencil[1],
                     stencil_writable ? " (want 0x11 kept, 0x5a written)" : "",
                     colour_ok && stencil_ok ? "PASS" : "FAIL");
        failures += colour_ok && stencil_ok ? 0 : 1;
    }
    std::fprintf(stderr, "%s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
