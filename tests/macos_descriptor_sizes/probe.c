#include <vulkan/vulkan.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(call) do { VkResult r = (call); if (r != VK_SUCCESS) { \
    fprintf(stderr, "%s: Vulkan result %d\n", #call, r); exit(1); } } while (0)
#define REQUIRE(condition) do { if (!(condition)) { \
    fprintf(stderr, "Requirement failed: %s\n", #condition); exit(1); } } while (0)

static double now_ns(void) {
    struct timespec t;
    REQUIRE(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
    return t.tv_sec * 1e9 + t.tv_nsec;
}
static int compare_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

int main(int argc, char **argv) {
    REQUIRE(argc >= 2);
    int use_push = 0, benchmark = 0;
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--push")) use_push = 1;
        else if (!strcmp(argv[i], "--bench")) benchmark = 1;
        else REQUIRE(0);
    }
    fprintf(stderr, "descriptor sizes: %s setup\n", use_push ? "push" : "allocated");
    FILE *file = fopen(argv[1], "rb"); REQUIRE(file);
    REQUIRE(fseek(file, 0, SEEK_END) == 0);
    long code_bytes = ftell(file); REQUIRE(code_bytes > 0 && !(code_bytes % 4));
    REQUIRE(fseek(file, 0, SEEK_SET) == 0);
    uint32_t *code = malloc(code_bytes); REQUIRE(code);
    REQUIRE(fread(code, 1, code_bytes, file) == (size_t)code_bytes);
    REQUIRE(fclose(file) == 0);
    VkApplicationInfo app = {.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName="MoltenVK descriptor size repro", .apiVersion=VK_API_VERSION_1_2};
    const char *ie[] = {VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME};
    VkInstanceCreateInfo ici = {.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .flags=VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR, .pApplicationInfo=&app,
        .enabledExtensionCount=1, .ppEnabledExtensionNames=ie};
    VkInstance instance; CHECK(vkCreateInstance(&ici, NULL, &instance));
    uint32_t count = 0; CHECK(vkEnumeratePhysicalDevices(instance, &count, NULL)); REQUIRE(count);
    VkPhysicalDevice *devices = calloc(count, sizeof(*devices)); REQUIRE(devices);
    CHECK(vkEnumeratePhysicalDevices(instance, &count, devices));
    VkPhysicalDevice physical = devices[0]; free(devices);
    VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(physical, &props);
    VkPhysicalDevice8BitStorageFeatures storage8 = {.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES};
    VkPhysicalDeviceFeatures2 features = {.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext=&storage8};
    vkGetPhysicalDeviceFeatures2(physical, &features); REQUIRE(storage8.storageBuffer8BitAccess);
    storage8.uniformAndStorageBuffer8BitAccess = VK_FALSE;
    storage8.storagePushConstant8 = VK_FALSE;
    uint32_t family_count = 0; vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, NULL);
    VkQueueFamilyProperties *families = calloc(family_count, sizeof(*families)); REQUIRE(families);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, families);
    uint32_t family = 0;
    while (family < family_count && !(families[family].queueFlags & VK_QUEUE_COMPUTE_BIT)) ++family;
    free(families); REQUIRE(family < family_count);
    float priority = 1;
    VkDeviceQueueCreateInfo qci = {.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex=family, .queueCount=1, .pQueuePriorities=&priority};
    const char *de[] = {"VK_KHR_portability_subset", VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME};
    VkDeviceCreateInfo dci = {.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext=&storage8,
        .queueCreateInfoCount=1, .pQueueCreateInfos=&qci, .enabledExtensionCount=2,
        .ppEnabledExtensionNames=de};
    VkDevice device; CHECK(vkCreateDevice(physical, &dci, NULL, &device));
    PFN_vkCmdPushDescriptorSetKHR push = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(device, "vkCmdPushDescriptorSetKHR");
    REQUIRE(push);
    VkQueue queue; vkGetDeviceQueue(device, family, 0, &queue);
    VkDeviceSize stride = props.limits.minStorageBufferOffsetAlignment;
    if (stride < 64) stride = 64;
    VkDeviceSize bytes = stride * 4;
    VkBuffer buffers[2]; VkDeviceMemory memory[2]; uint32_t *mapped[2];
    for (unsigned i = 0; i < 2; ++i) {
        VkBufferCreateInfo bci = {.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size=bytes, .usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
        CHECK(vkCreateBuffer(device, &bci, NULL, &buffers[i]));
        VkMemoryRequirements mr; vkGetBufferMemoryRequirements(device, buffers[i], &mr);
        VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(physical, &mp);
        uint32_t type = 0, flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        while (type < mp.memoryTypeCount && (!(mr.memoryTypeBits & (1u << type)) ||
            (mp.memoryTypes[type].propertyFlags & flags) != flags)) ++type;
        REQUIRE(type < mp.memoryTypeCount);
        VkMemoryAllocateInfo mai = {.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize=mr.size, .memoryTypeIndex=type};
        CHECK(vkAllocateMemory(device, &mai, NULL, &memory[i]));
        CHECK(vkBindBufferMemory(device, buffers[i], memory[i], 0));
        CHECK(vkMapMemory(device, memory[i], 0, bytes, 0, (void **)&mapped[i]));
        for (VkDeviceSize j = 0; j < bytes / 4; ++j) mapped[i][j] = i ? 0xcafef00d : 0x1000 + (uint32_t)j;
    }
    VkDescriptorSetLayoutBinding bindings[3];
    for (unsigned i = 0; i < 3; ++i) bindings[i] = (VkDescriptorSetLayoutBinding){
        .binding=i, .descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount=1, .stageFlags=VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayout layouts[2]; VkPipelineLayout pl[2];
    for (unsigned i = 0; i < 2; ++i) {
        VkDescriptorSetLayoutCreateInfo lci = {.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .flags=i ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR : 0,
            .bindingCount=3, .pBindings=bindings};
        CHECK(vkCreateDescriptorSetLayout(device, &lci, NULL, &layouts[i]));
        VkPipelineLayoutCreateInfo pci = {.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount=1, .pSetLayouts=&layouts[i]};
        CHECK(vkCreatePipelineLayout(device, &pci, NULL, &pl[i]));
    }
    enum { BATCH=32, DRAWS=1024, SAMPLES=21 };
    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 * DRAWS};
    VkDescriptorPoolCreateInfo dpci = {.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets=DRAWS, .poolSizeCount=1, .pPoolSizes=&ps};
    VkDescriptorPool pool; CHECK(vkCreateDescriptorPool(device, &dpci, NULL, &pool));
    VkDescriptorSetAllocateInfo sai = {.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool=pool, .descriptorSetCount=1, .pSetLayouts=&layouts[0]};
    VkDescriptorSet set; CHECK(vkAllocateDescriptorSets(device, &sai, &set));
    VkCommandPoolCreateInfo cpci = {.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex=family, .flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
    VkCommandPool cp; CHECK(vkCreateCommandPool(device, &cpci, NULL, &cp));
    VkCommandBufferAllocateInfo cai = {.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool=cp, .level=VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount=1};
    VkCommandBuffer cmd; CHECK(vkAllocateCommandBuffers(device, &cai, &cmd));
    VkShaderModuleCreateInfo smci = {.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize=code_bytes, .pCode=code};
    VkShaderModule module; CHECK(vkCreateShaderModule(device, &smci, NULL, &module)); free(code);
    VkComputePipelineCreateInfo pci = {.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage=VK_SHADER_STAGE_COMPUTE_BIT, .module=module, .pName="main"}, .layout=pl[use_push]};
    VkPipeline pipeline; CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pci, NULL, &pipeline));
    VkDescriptorBufferInfo bi[3] = {{buffers[0], 0, 36}, {buffers[0], stride, 37}, {buffers[1], 0, bytes}};
    VkWriteDescriptorSet writes[3];
    for (unsigned i = 0; i < 3; ++i) writes[i] = (VkWriteDescriptorSet){
        .sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet=set, .dstBinding=i,
        .descriptorCount=1, .descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo=&bi[i]};
    const uint32_t sizes[] = {36, 37, 5, 7, 16, 63, 32, 35, 4, 11, 36, 37};
    int failures = 0;
    for (unsigned i = 0; i < sizeof(sizes)/sizeof(*sizes); ++i) {
        bi[0].range = sizes[i]; bi[1].range = sizes[11-i];
        bi[0].offset = (i & 1) * stride; bi[1].offset = (1 + (i & 1)) * stride;
        for (unsigned j = 0; j < 5; ++j) mapped[1][j] = 0xcafef00d;
        if (!use_push) vkUpdateDescriptorSets(device, 3, writes, 0, NULL);
        VkCommandBufferBeginInfo begin = {.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        CHECK(vkBeginCommandBuffer(cmd, &begin));
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        if (use_push) push(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl[1], 0, 3, writes);
        else vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl[0], 0, 1, &set, 0, NULL);
        vkCmdDispatch(cmd, 1, 1, 1);
        VkMemoryBarrier barrier = {.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask=VK_ACCESS_HOST_READ_BIT};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
            0, 1, &barrier, 0, NULL, 0, NULL);
        CHECK(vkEndCommandBuffer(cmd));
        VkSubmitInfo submit = {.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount=1, .pCommandBuffers=&cmd};
        CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE)); CHECK(vkQueueWaitIdle(queue));
        uint32_t expected[] = {sizes[i], sizes[i]/4, sizes[11-i]/4,
            0x1000+(uint32_t)(bi[0].offset/4), 0x1000+(uint32_t)(bi[1].offset/4)};
        fprintf(stderr, "case %u ranges %u/%u: byte=%u dword=%u other=%u reads=%#x/%#x\n",
            i, sizes[i], sizes[11-i], mapped[1][0], mapped[1][1], mapped[1][2], mapped[1][3], mapped[1][4]);
        for (unsigned j = 0; j < 5; ++j) if (mapped[1][j] != expected[j]) ++failures;
        CHECK(vkResetCommandBuffer(cmd, 0));
    }
    fprintf(stderr, "descriptor size/data checks: %s (%d mismatches)\n", failures ? "FAIL" : "PASS", failures);
    if (benchmark) {
        double samples[2][SAMPLES];
        VkDescriptorSetLayout batch_layouts[BATCH];
        for (unsigned j=0; j<BATCH; ++j) batch_layouts[j]=layouts[0];
        for (unsigned repetition=0; repetition<SAMPLES; ++repetition) for (unsigned order=0; order<2; ++order) {
            unsigned mode=(repetition+order)&1;
            CHECK(vkResetCommandBuffer(cmd, 0)); CHECK(vkResetDescriptorPool(device, pool, 0));
            VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            CHECK(vkBeginCommandBuffer(cmd, &begin));
            VkDescriptorSet sets[BATCH];
            double start=now_ns();
            for (unsigned draw=0; draw<DRAWS; ++draw) {
                if (mode) push(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl[1], 0, 3, writes);
                else {
                    if (!(draw%BATCH)) {
                        sai.descriptorSetCount=BATCH; sai.pSetLayouts=batch_layouts;
                        CHECK(vkAllocateDescriptorSets(device, &sai, sets));
                    }
                    VkDescriptorSet ds=sets[draw%BATCH];
                    for (unsigned j=0; j<3; ++j) writes[j].dstSet=ds;
                    vkUpdateDescriptorSets(device, 3, writes, 0, NULL);
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl[0], 0, 1, &ds, 0, NULL);
                }
            }
            samples[mode][repetition]=(now_ns()-start)/DRAWS;
            CHECK(vkEndCommandBuffer(cmd));
        }
        for (unsigned mode=0; mode<2; ++mode) {
            qsort(samples[mode], SAMPLES, sizeof(double), compare_double);
            printf("CPU %s descriptor record ns/draw median=%.1f IQR=%.1f..%.1f; 200 draws=%.3f ms\n",
                mode?"push":"allocated batch32/update/bind", samples[mode][10], samples[mode][5],
                samples[mode][15], samples[mode][10]*200/1e6);
        }
    }
    vkDestroyPipeline(device, pipeline, NULL); vkDestroyShaderModule(device, module, NULL);
    vkDestroyCommandPool(device, cp, NULL); vkDestroyDescriptorPool(device, pool, NULL);
    for (unsigned i=0; i<2; ++i) {
        vkDestroyPipelineLayout(device, pl[i], NULL); vkDestroyDescriptorSetLayout(device, layouts[i], NULL);
        vkUnmapMemory(device, memory[i]); vkDestroyBuffer(device, buffers[i], NULL); vkFreeMemory(device, memory[i], NULL);
    }
    vkDestroyDevice(device, NULL); vkDestroyInstance(instance, NULL);
    return failures ? 2 : 0;
}
