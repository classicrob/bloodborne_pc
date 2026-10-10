// Opus-GS design measurement: what a compute pre-pass costs when it has to end and resume the
// current render pass on MoltenVK (Apple TBDR stores every attachment at the end of a pass and
// loads it again at the start of the next). Attachment set like the game's G-buffer: four
// RGBA16F colour targets plus D32S8.
//   base:  one pass, K draws
//   break: K x (begin LOAD/STORE, draw, end)               -- pass boundaries only
//   gs:    K x (begin, draw, end, barrier, dispatch, barrier) -- what a GS pre-pass does
// Timing is per submission (first/last timestamp of a command buffer), the only attribution
// that MoltenVK 1.4.2 gets right on the M4 (see the perf harness notes).
#include <vulkan/vulkan.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) { \
    fprintf(stderr, "%s: Vulkan result %d\n", #call, r_); exit(1); } } while (0)
#define REQUIRE(c) do { if (!(c)) { fprintf(stderr, "Requirement failed: %s\n", #c); exit(1); } } while (0)

enum { COLORS = 4, K = 16, SAMPLES = 9 };

static VkDevice device;
static VkPhysicalDevice physical;
static VkPhysicalDeviceMemoryProperties memory_props;

static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags flags) {
    for (uint32_t i = 0; i < memory_props.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (memory_props.memoryTypes[i].propertyFlags & flags) == flags) return i;
    REQUIRE(!"memory type");
    return 0;
}

static VkShaderModule load_module(const char *path) {
    FILE *f = fopen(path, "rb");
    REQUIRE(f);
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint32_t *code = malloc(size);
    REQUIRE(fread(code, 1, size, f) == (size_t)size);
    fclose(f);
    const VkShaderModuleCreateInfo info = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = (size_t)size, .pCode = code};
    VkShaderModule module;
    CHECK(vkCreateShaderModule(device, &info, NULL, &module));
    free(code);
    return module;
}

static VkImage make_image(VkFormat format, VkImageUsageFlags usage, uint32_t w, uint32_t h, VkImageView *view,
                          VkImageAspectFlags aspect) {
    const VkImageCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = format, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage image;
    CHECK(vkCreateImage(device, &info, NULL, &image));
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(device, image, &mr);
    const VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
        .memoryTypeIndex = memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    VkDeviceMemory memory;
    CHECK(vkAllocateMemory(device, &ai, NULL, &memory));
    CHECK(vkBindImageMemory(device, image, memory, 0));
    const VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = format, .subresourceRange = {aspect, 0, 1, 0, 1}};
    CHECK(vkCreateImageView(device, &vi, NULL, view));
    return image;
}

int main(int argc, char **argv) {
    REQUIRE(argc == 5);
    const char *dir = argv[1];
    const uint32_t width = (uint32_t)atoi(argv[2]), height = (uint32_t)atoi(argv[3]);
    const int32_t full = atoi(argv[4]);

    const VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
    const char *instance_extensions[] = {VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME};
    const VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR, .pApplicationInfo = &app,
        .enabledExtensionCount = 1, .ppEnabledExtensionNames = instance_extensions};
    VkInstance instance;
    CHECK(vkCreateInstance(&ici, NULL, &instance));
    uint32_t count = 1;
    vkEnumeratePhysicalDevices(instance, &count, &physical);
    REQUIRE(count == 1);
    vkGetPhysicalDeviceMemoryProperties(physical, &memory_props);
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physical, &props);

    const float priority = 1.0f;
    const VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &priority};
    VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .dynamicRendering = VK_TRUE, .synchronization2 = VK_TRUE};
    const char *device_extensions[] = {"VK_KHR_portability_subset"};
    const VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f13,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci, .enabledExtensionCount = 1,
        .ppEnabledExtensionNames = device_extensions};
    CHECK(vkCreateDevice(physical, &dci, NULL, &device));
    VkQueue queue;
    vkGetDeviceQueue(device, 0, 0, &queue);

    VkImageView color_views[COLORS], depth_view;
    VkImage colors[COLORS], depth;
    const VkFormat color_format = VK_FORMAT_R16G16B16A16_SFLOAT, depth_format = VK_FORMAT_D32_SFLOAT_S8_UINT;
    for (int i = 0; i < COLORS; ++i)
        colors[i] = make_image(color_format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                               width, height, &color_views[i], VK_IMAGE_ASPECT_COLOR_BIT);
    depth = make_image(depth_format, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                       width, height, &depth_view, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);

    // Storage buffer for the dispatch.
    const VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 1 << 20,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    VkBuffer buffer;
    CHECK(vkCreateBuffer(device, &bci, NULL, &buffer));
    VkMemoryRequirements bmr;
    vkGetBufferMemoryRequirements(device, buffer, &bmr);
    const VkMemoryAllocateInfo bai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = bmr.size,
        .memoryTypeIndex = memory_type(bmr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    VkDeviceMemory bmem;
    CHECK(vkAllocateMemory(device, &bai, NULL, &bmem));
    CHECK(vkBindBufferMemory(device, buffer, bmem, 0));

    // Pipelines.
    char path[1024];
    snprintf(path, sizeof path, "%s/tri.vert.spv", dir);
    const VkShaderModule vs = load_module(path);
    snprintf(path, sizeof path, "%s/mrt.frag.spv", dir);
    const VkShaderModule fs = load_module(path);
    snprintf(path, sizeof path, "%s/touch.comp.spv", dir);
    const VkShaderModule cs = load_module(path);

    const VkDescriptorSetLayoutBinding binding = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    const VkDescriptorSetLayoutCreateInfo dslci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1, .pBindings = &binding};
    VkDescriptorSetLayout dsl;
    CHECK(vkCreateDescriptorSetLayout(device, &dslci, NULL, &dsl));
    const VkPipelineLayoutCreateInfo cplci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &dsl};
    VkPipelineLayout compute_layout, graphics_layout;
    CHECK(vkCreatePipelineLayout(device, &cplci, NULL, &compute_layout));
    const VkPipelineLayoutCreateInfo gplci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    CHECK(vkCreatePipelineLayout(device, &gplci, NULL, &graphics_layout));
    const VkComputePipelineCreateInfo cpci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                  .module = cs, .pName = "main"}, .layout = compute_layout};
    VkPipeline compute;
    CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpci, NULL, &compute));

    const VkSpecializationMapEntry spec_entry = {0, 0, sizeof(int32_t)};
    const VkSpecializationInfo spec = {1, &spec_entry, sizeof(int32_t), &full};
    const VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main",
         .pSpecializationInfo = &spec},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main"}};
    const VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    const VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    const VkViewport viewport = {0, 0, (float)width, (float)height, 0, 1};
    const VkRect2D scissor = {{0, 0}, {width, height}};
    const VkPipelineViewportStateCreateInfo vps = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .pViewports = &viewport, .scissorCount = 1, .pScissors = &scissor};
    const VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1.0f};
    const VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    const VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_TRUE, .depthWriteEnable = VK_TRUE, .depthCompareOp = VK_COMPARE_OP_ALWAYS};
    VkPipelineColorBlendAttachmentState blend[COLORS];
    VkFormat color_formats[COLORS];
    for (int i = 0; i < COLORS; ++i) {
        blend[i] = (VkPipelineColorBlendAttachmentState){.colorWriteMask = 0xf};
        color_formats[i] = color_format;
    }
    const VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = COLORS, .pAttachments = blend};
    const VkPipelineRenderingCreateInfo rci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = COLORS, .pColorAttachmentFormats = color_formats,
        .depthAttachmentFormat = depth_format, .stencilAttachmentFormat = depth_format};
    const VkGraphicsPipelineCreateInfo gpci = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &rci,
        .stageCount = 2, .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
        .pViewportState = &vps, .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
        .pColorBlendState = &cb, .layout = graphics_layout};
    VkPipeline graphics;
    CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpci, NULL, &graphics));

    const VkDescriptorPoolSize pool_size = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    const VkDescriptorPoolCreateInfo dpci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1,
        .poolSizeCount = 1, .pPoolSizes = &pool_size};
    VkDescriptorPool dpool;
    CHECK(vkCreateDescriptorPool(device, &dpci, NULL, &dpool));
    const VkDescriptorSetAllocateInfo dsai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = dpool, .descriptorSetCount = 1, .pSetLayouts = &dsl};
    VkDescriptorSet set;
    CHECK(vkAllocateDescriptorSets(device, &dsai, &set));
    const VkDescriptorBufferInfo dbi = {buffer, 0, VK_WHOLE_SIZE};
    const VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
        .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi};
    vkUpdateDescriptorSets(device, 1, &write, 0, NULL);

    const VkCommandPoolCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
    VkCommandPool cpool;
    CHECK(vkCreateCommandPool(device, &cpi, NULL, &cpool));
    const VkCommandBufferAllocateInfo cbai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = cpool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer cmd;
    CHECK(vkAllocateCommandBuffers(device, &cbai, &cmd));
    const VkQueryPoolCreateInfo qpci = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2};
    VkQueryPool qpool;
    CHECK(vkCreateQueryPool(device, &qpci, NULL, &qpool));
    const VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    CHECK(vkCreateFence(device, &fci, NULL, &fence));

    // Layout transitions once.
    {
        const VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        CHECK(vkBeginCommandBuffer(cmd, &begin));
        VkImageMemoryBarrier barriers[COLORS + 1];
        for (int i = 0; i <= COLORS; ++i) {
            const int is_depth = i == COLORS;
            barriers[i] = (VkImageMemoryBarrier){.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .dstAccessMask = is_depth ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .newLayout = is_depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = is_depth ? depth : colors[i],
                .subresourceRange = {is_depth ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        }
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, 0, 0, NULL, 0,
                             NULL, COLORS + 1, barriers);
        CHECK(vkEndCommandBuffer(cmd));
        const VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd};
        CHECK(vkQueueSubmit(queue, 1, &si, fence));
        CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
        CHECK(vkResetFences(device, 1, &fence));
    }

    VkRenderingAttachmentInfo color_attachments[COLORS];
    for (int i = 0; i < COLORS; ++i)
        color_attachments[i] = (VkRenderingAttachmentInfo){.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = color_views[i], .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    const VkRenderingAttachmentInfo depth_attachment = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = depth_view, .imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    const VkRenderingInfo rendering = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = scissor,
        .layerCount = 1, .colorAttachmentCount = COLORS, .pColorAttachments = color_attachments,
        .pDepthAttachment = &depth_attachment, .pStencilAttachment = &depth_attachment};
    const VkMemoryBarrier to_compute = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
    const VkMemoryBarrier to_vertex = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT};

    const char *names[3] = {"base", "break", "gs"};
    double medians[3];
    for (int mode = 0; mode < 3; ++mode) {
        double samples[SAMPLES];
        for (int s = 0; s < SAMPLES + 1; ++s) {
            const VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
            CHECK(vkResetCommandBuffer(cmd, 0));
            CHECK(vkBeginCommandBuffer(cmd, &begin));
            vkCmdResetQueryPool(cmd, qpool, 0, 2);
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, qpool, 0);
            if (mode == 0) vkCmdBeginRendering(cmd, &rendering);
            for (int k = 0; k < K; ++k) {
                if (mode != 0) vkCmdBeginRendering(cmd, &rendering);
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, graphics);
                vkCmdDraw(cmd, 3, 1, 0, 0);
                if (mode != 0) vkCmdEndRendering(cmd);
                if (mode == 2) {
                    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                         0, 1, &to_compute, 0, NULL, 0, NULL);
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, compute);
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, compute_layout, 0, 1, &set, 0, NULL);
                    vkCmdDispatch(cmd, 16, 1, 1);
                    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                         VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                                             VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
                                         0, 1, &to_vertex, 0, NULL, 0, NULL);
                }
            }
            if (mode == 0) vkCmdEndRendering(cmd);
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, qpool, 1);
            CHECK(vkEndCommandBuffer(cmd));
            const VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd};
            CHECK(vkQueueSubmit(queue, 1, &si, fence));
            CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
            CHECK(vkResetFences(device, 1, &fence));
            uint64_t ts[2];
            CHECK(vkGetQueryPoolResults(device, qpool, 0, 2, sizeof ts, ts, sizeof ts[0],
                                        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
            if (s > 0) samples[s - 1] = (double)(ts[1] - ts[0]) * props.limits.timestampPeriod / 1e6;
        }
        for (int i = 0; i < SAMPLES; ++i)
            for (int j = i + 1; j < SAMPLES; ++j)
                if (samples[j] < samples[i]) { const double t = samples[i]; samples[i] = samples[j]; samples[j] = t; }
        medians[mode] = samples[SAMPLES / 2];
        printf("%ux%u %s %-5s: %.3f ms per submission (min %.3f max %.3f)\n", width, height, full ? "full " : "small", names[mode],
               medians[mode], samples[0], samples[SAMPLES - 1]);
    }
    const double bytes = (double)width * height * (COLORS * 8 + 5);
    printf("%ux%u %s per pass boundary: %.3f ms; with GS-style dispatch + barriers: %.3f ms "
           "(attachment set %.1f MB, store+load %.1f MB)\n",
           width, height, full ? "full " : "small", (medians[1] - medians[0]) / K, (medians[2] - medians[0]) / K, bytes / 1e6, 2 * bytes / 1e6);
    return 0;
}
