// Qualifies MoltenVK GPU timestamps for BB_PERF (docs/macos/perf.md). The contract BB_PERF
// relies on: the first and last timestamp of each command buffer (vkCmdWriteTimestamp,
// ALL_COMMANDS, query ranges reused like a ring) bracket that submission's GPU work. Checked with
// compute and transfer work: x2/x4 work takes longer, an empty submission is near zero, a repeat
// is not shorter, the GPU time stays within the CPU wall time of submit + wait, and serialized
// submissions are ordered on one timebase.
// Diagnostic only (printed, not asserted): timestamps between compute dispatches inside one
// command buffer do NOT attribute work correctly on MoltenVK 1.4.2 / M4 Max, so BB_PERF never
// uses them.
#include <vulkan/vulkan.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(call) do { VkResult r = (call); if (r != VK_SUCCESS) { \
    fprintf(stderr, "%s: Vulkan result %d\n", #call, r); exit(1); } } while (0)
#define REQUIRE(condition) do { if (!(condition)) { \
    fprintf(stderr, "Requirement failed: %s\n", #condition); exit(1); } } while (0)

enum { GROUPS = 1024, SEGMENTS = 5, SAMPLES = 11 };
// Work per segment, in units of the calibrated iteration count; segment 3 is empty.
static const unsigned scale[SEGMENTS] = {1, 2, 4, 0, 1};

static double now_ms(void) {
    struct timespec t;
    REQUIRE(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}
static int compare_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}
static double minimum(const double *values, unsigned count) {
    double low = values[0];
    for (unsigned i = 1; i < count; ++i) low = values[i] < low ? values[i] : low;
    return low;
}
static double median(double *values, unsigned count) {
    qsort(values, count, sizeof(*values), compare_double);
    return values[count / 2];
}

static VkDevice device;
static VkQueue queue;
static VkCommandBuffer command;
static VkFence fence;
static VkQueryPool pool;
static VkPipeline pipeline;
static VkPipelineLayout layout;
static VkDescriptorSet set;
static VkBuffer fill_buffer;
static double period;
static uint32_t valid_bits;
static uint64_t last_end; // end timestamp of the previous serialized submission

// Diagnostic: one submission, SEGMENTS + 1 timestamps between compute segments (or fills).
// Returns GPU ms per segment and the CPU wall time of submit + wait.
static double run_in_buffer(unsigned iterations, int fill, unsigned base, double *segments) {
    const VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    CHECK(vkResetCommandBuffer(command, 0));
    CHECK(vkBeginCommandBuffer(command, &begin));
    vkCmdResetQueryPool(command, pool, base, SEGMENTS + 1);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, NULL);
    const VkMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT};
    for (unsigned i = 0; i < SEGMENTS; ++i) {
        vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, pool, base + i);
        if (scale[i]) {
            if (fill) {
                vkCmdFillBuffer(command, fill_buffer, 0, (VkDeviceSize)iterations * scale[i], 0x5a5a5a5a);
            } else {
                const uint32_t count = iterations * scale[i];
                vkCmdPushConstants(command, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(count), &count);
                vkCmdDispatch(command, GROUPS, 1, 1);
            }
        }
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
    }
    vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, pool, base + SEGMENTS);
    CHECK(vkEndCommandBuffer(command));
    const VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &command};
    const double start = now_ms();
    CHECK(vkQueueSubmit(queue, 1, &submit, fence));
    CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, 10000000000ull));
    const double wall = now_ms() - start;
    CHECK(vkResetFences(device, 1, &fence));
    struct { uint64_t value, available; } stamps[SEGMENTS + 1];
    CHECK(vkGetQueryPoolResults(device, pool, base, SEGMENTS + 1, sizeof(stamps), stamps,
        sizeof(stamps[0]), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT));
    const uint64_t mask = valid_bits < 64 ? (UINT64_C(1) << valid_bits) - 1 : UINT64_MAX;
    for (unsigned i = 0; i < SEGMENTS; ++i) {
        REQUIRE(stamps[i].available && stamps[i + 1].available);
        REQUIRE(stamps[i + 1].value >= stamps[i].value);
        segments[i] = (double)((stamps[i + 1].value - stamps[i].value) & mask) * period / 1e6;
    }
    return wall;
}

// The BB_PERF contract: each segment is its own submission, timestamped first and last.
static double run_per_submission(unsigned iterations, int fill, unsigned base, double *segments) {
    const VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    double wall = 0;
    for (unsigned i = 0; i < SEGMENTS; ++i) {
        CHECK(vkResetCommandBuffer(command, 0));
        CHECK(vkBeginCommandBuffer(command, &begin));
        vkCmdResetQueryPool(command, pool, base, 2);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, NULL);
        vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, pool, base);
        if (scale[i]) {
            if (fill) {
                vkCmdFillBuffer(command, fill_buffer, 0, (VkDeviceSize)iterations * scale[i], 0x5a5a5a5a);
            } else {
                const uint32_t count = iterations * scale[i];
                vkCmdPushConstants(command, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(count), &count);
                vkCmdDispatch(command, GROUPS, 1, 1);
            }
        }
        vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, pool, base + 1);
        CHECK(vkEndCommandBuffer(command));
        const VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1, .pCommandBuffers = &command};
        const double start = now_ms();
        CHECK(vkQueueSubmit(queue, 1, &submit, fence));
        CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, 10000000000ull));
        wall += now_ms() - start;
        CHECK(vkResetFences(device, 1, &fence));
        struct { uint64_t value, available; } stamps[2];
        CHECK(vkGetQueryPoolResults(device, pool, base, 2, sizeof(stamps), stamps,
            sizeof(stamps[0]), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT));
        REQUIRE(stamps[0].available && stamps[1].available && stamps[1].value >= stamps[0].value);
        // One GPU timebase across submissions: BB_PERF merges spans of both streams by time.
        REQUIRE(stamps[0].value >= last_end);
        last_end = stamps[1].value;
        segments[i] = (double)(stamps[1].value - stamps[0].value) * period / 1e6;
    }
    return wall;
}

typedef double (*Runner)(unsigned iterations, int fill, unsigned base, double *segments);

// Fastest per-segment times over SAMPLES runs, alternating query ranges like the ring. Other GPU
// users and clock changes only add time, so the minimum is the estimate least disturbed by them.
static double measure(Runner run, unsigned iterations, int fill, double *segments) {
    double samples[SEGMENTS][SAMPLES], walls[SAMPLES];
    for (unsigned s = 0; s < SAMPLES; ++s) {
        double one[SEGMENTS];
        walls[s] = run(iterations, fill, (s % 4) * (SEGMENTS + 1), one);
        for (unsigned i = 0; i < SEGMENTS; ++i) samples[i][s] = one[i];
    }
    for (unsigned i = 0; i < SEGMENTS; ++i) segments[i] = minimum(samples[i], SAMPLES);
    return median(walls, SAMPLES);
}

static int check(const char *what, const double *ms, double wall, double base_ms, int assert_contract) {
    double total = 0;
    for (unsigned i = 0; i < SEGMENTS; ++i) total += ms[i];
    const double r2 = ms[1] / ms[0], r4 = ms[2] / ms[0], repeat = ms[4] / ms[0];
    printf("%s: unit %.3f ms, x2 %.3f (%.2fx), x4 %.3f (%.2fx), empty %.4f ms, repeat %.3f (%.2fx), "
           "GPU sum %.3f ms, CPU wall %.3f ms\n",
           what, ms[0], ms[1], r2, ms[2], r4, ms[3], ms[4], repeat, total, wall);
    if (!assert_contract) return 1;
    int ok = 1;
    if (!(ms[0] >= base_ms)) { fprintf(stderr, "%s: unit segment below %.2f ms\n", what, base_ms); ok = 0; }
    // One-sided bounds. --test shares the GPU with other work (a running game, other tests),
    // and interference only adds time to a segment: under load x2 and x4 compute reached 2.8x
    // and 6.5x and a repeat 1.5x, all real GPU time. The failure this guards against makes
    // segments too SHORT or misplaced: in-buffer compute timestamps give an empty segment
    // 0.26-2.7 ms (5-30% of the unit), a repeat of 0.0-0.22x and x2/x4 as low as 0.33x/2.2x.
    // Spans that do not follow the work at all read about 1x. The empty-segment check is the
    // sharpest; the lower bounds catch spans that stop scaling.
    if (!(r2 > 1.2)) { fprintf(stderr, "%s: x2 work did not scale\n", what); ok = 0; }
    if (!(r4 > 2.0)) { fprintf(stderr, "%s: x4 work did not scale\n", what); ok = 0; }
    if (!(repeat > 0.5)) { fprintf(stderr, "%s: repeated segment is too short\n", what); ok = 0; }
    if (!(ms[3] < 0.05 * ms[0] && ms[3] < 0.1)) { fprintf(stderr, "%s: empty segment is not empty\n", what); ok = 0; }
    if (!(total <= wall * 1.02 + 0.05)) { fprintf(stderr, "%s: GPU time exceeds CPU wall time\n", what); ok = 0; }
    return ok;
}

int main(int argc, char **argv) {
    REQUIRE(argc == 2);
    FILE *file = fopen(argv[1], "rb"); REQUIRE(file);
    REQUIRE(fseek(file, 0, SEEK_END) == 0);
    const long code_bytes = ftell(file); REQUIRE(code_bytes > 0 && !(code_bytes % 4));
    REQUIRE(fseek(file, 0, SEEK_SET) == 0);
    uint32_t *code = malloc(code_bytes); REQUIRE(code);
    REQUIRE(fread(code, 1, code_bytes, file) == (size_t)code_bytes);
    REQUIRE(fclose(file) == 0);

    const VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "MoltenVK timestamp qualification", .apiVersion = VK_API_VERSION_1_2};
    const char *instance_extensions[] = {VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME};
    const VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR, .pApplicationInfo = &app,
        .enabledExtensionCount = 1, .ppEnabledExtensionNames = instance_extensions};
    VkInstance instance; CHECK(vkCreateInstance(&ici, NULL, &instance));
    uint32_t count = 1; VkPhysicalDevice physical;
    VkResult enumerated = vkEnumeratePhysicalDevices(instance, &count, &physical);
    REQUIRE((enumerated == VK_SUCCESS || enumerated == VK_INCOMPLETE) && count == 1);
    VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(physical, &props);
    period = props.limits.timestampPeriod;
    REQUIRE(props.limits.timestampComputeAndGraphics);
    REQUIRE(isfinite(period) && period > 0);
    // The game's queue: the first graphics family (vk_instance.cpp picks it the same way).
    uint32_t family_count = 0; vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, NULL);
    VkQueueFamilyProperties families[16]; REQUIRE(family_count && family_count <= 16);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, families);
    uint32_t family = 0;
    while (family < family_count && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) ++family;
    REQUIRE(family < family_count && (families[family].queueFlags & VK_QUEUE_COMPUTE_BIT));
    valid_bits = families[family].timestampValidBits;
    REQUIRE(valid_bits >= 36);
    printf("device=%s timestampPeriod=%.4f ns validBits=%u family=%u\n",
           props.deviceName, period, valid_bits, family);

    const float priority = 1;
    const VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority};
    const char *device_extensions[] = {"VK_KHR_portability_subset"};
    VkPhysicalDeviceVulkan12Features v12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .hostQueryReset = VK_TRUE};
    const VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &v12,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci, .enabledExtensionCount = 1,
        .ppEnabledExtensionNames = device_extensions};
    CHECK(vkCreateDevice(physical, &dci, NULL, &device));
    vkGetDeviceQueue(device, family, 0, &queue);

    const VkDeviceSize output_bytes = GROUPS * 64 * sizeof(uint32_t), fill_bytes = 256ull << 20;
    VkPhysicalDeviceMemoryProperties memory_props; vkGetPhysicalDeviceMemoryProperties(physical, &memory_props);
    VkBuffer output;
    VkBuffer *targets[2] = {&output, &fill_buffer};
    const VkDeviceSize sizes[2] = {output_bytes, fill_bytes};
    for (unsigned i = 0; i < 2; ++i) {
        const VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = sizes[i],
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        CHECK(vkCreateBuffer(device, &bci, NULL, targets[i]));
        VkMemoryRequirements mr; vkGetBufferMemoryRequirements(device, *targets[i], &mr);
        uint32_t type = 0;
        while (type < memory_props.memoryTypeCount && (!(mr.memoryTypeBits & (1u << type)) ||
               !(memory_props.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))) ++type;
        REQUIRE(type < memory_props.memoryTypeCount);
        const VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = mr.size, .memoryTypeIndex = type};
        VkDeviceMemory memory; CHECK(vkAllocateMemory(device, &mai, NULL, &memory));
        CHECK(vkBindBufferMemory(device, *targets[i], memory, 0));
    }

    const VkDescriptorSetLayoutBinding binding = {.binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    const VkDescriptorSetLayoutCreateInfo lci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1, .pBindings = &binding};
    VkDescriptorSetLayout set_layout; CHECK(vkCreateDescriptorSetLayout(device, &lci, NULL, &set_layout));
    const VkPushConstantRange range = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(uint32_t)};
    const VkPipelineLayoutCreateInfo plci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &range};
    CHECK(vkCreatePipelineLayout(device, &plci, NULL, &layout));
    const VkShaderModuleCreateInfo smci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = (size_t)code_bytes, .pCode = code};
    VkShaderModule module; CHECK(vkCreateShaderModule(device, &smci, NULL, &module));
    const VkComputePipelineCreateInfo cpci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main"},
        .layout = layout};
    CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpci, NULL, &pipeline));
    const VkDescriptorPoolSize pool_size = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    const VkDescriptorPoolCreateInfo dpci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &pool_size};
    VkDescriptorPool descriptor_pool; CHECK(vkCreateDescriptorPool(device, &dpci, NULL, &descriptor_pool));
    const VkDescriptorSetAllocateInfo dsai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = descriptor_pool, .descriptorSetCount = 1, .pSetLayouts = &set_layout};
    CHECK(vkAllocateDescriptorSets(device, &dsai, &set));
    const VkDescriptorBufferInfo dbi = {output, 0, output_bytes};
    const VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
        .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi};
    vkUpdateDescriptorSets(device, 1, &write, 0, NULL);

    const VkQueryPoolCreateInfo qpci = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 4 * (SEGMENTS + 1)};
    CHECK(vkCreateQueryPool(device, &qpci, NULL, &pool));
    const VkCommandPoolCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = family};
    VkCommandPool command_pool; CHECK(vkCreateCommandPool(device, &cpi, NULL, &command_pool));
    const VkCommandBufferAllocateInfo cbai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = command_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    CHECK(vkAllocateCommandBuffers(device, &cbai, &command));
    const VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    CHECK(vkCreateFence(device, &fci, NULL, &fence));

    // Calibrate the unit segment to at least 1 ms of GPU time. Short segments are less likely to
    // be time-sliced with other GPU users. The calibration doubles as warm-up; the clock still
    // varies, so the measured unit only has to stay above 0.4 ms, enough signal for the ratios.
    double ms[SEGMENTS];
    unsigned iterations = 256;
    for (;;) {
        measure(run_per_submission, iterations, 0, ms);
        if (ms[0] >= 1.0 || iterations >= (1u << 24)) break;
        iterations *= 2;
    }
    for (unsigned i = 0; i < 4; ++i) measure(run_per_submission, iterations, 0, ms);
    int ok = check("per-submission compute", ms, measure(run_per_submission, iterations, 0, ms), 0.4, 1);
    // Transfer work (Metal blit encoders): 64/128/256 MiB fills.
    ok &= check("per-submission fill", ms, measure(run_per_submission, 64u << 20, 1, ms), 0.05, 1);
    printf("MEASURED (diagnostic, not used by BB_PERF):\n");
    check("  in-buffer compute", ms, measure(run_in_buffer, iterations, 0, ms), 0, 0);
    check("  in-buffer fill", ms, measure(run_in_buffer, 64u << 20, 1, ms), 0, 0);
    if (!ok) {
        fprintf(stderr, "MoltenVK timestamps do not measure GPU work; BB_PERF results would be invalid\n");
        return 1;
    }
    puts("MoltenVK per-submission timestamps: scaling, empty, repeat, ring reuse, wall-time bound PASS");
    return 0;
}
