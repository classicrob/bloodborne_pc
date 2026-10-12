/* Runtime-fed arithmetic bit patterns through actual MoltenVK compute. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#include "../test_alarm.h"
#define CHECK(call) do { VkResult r_=(call); if(r_) { fprintf(stderr,"FAIL %s: %d\n",#call,r_); exit(1); } } while(0)
static VkDevice device;
static VkPhysicalDevice physical;
static VkQueue queue;
static VkCommandPool commands;
static VkPipeline pipeline;
static VkPipelineLayout layout;
static VkPhysicalDeviceMemoryProperties memory_properties;
typedef struct { const char *name; uint32_t op,a,b,c; } Case;
#if defined(BB_FP16_CASES) || defined(BB_PKRTZ_CASES)
#ifdef BB_PKRTZ_CASES
#include "../macos_pkrtz/cases.h"
#else
#include "../macos_fp16/cases.h"
#endif
#else
static const Case cases[]={
    {"add_smallest_subnormals",0,0x00000001,0x00000001,0},
    {"add_subnormals_to_normal",0,0x007fffff,0x00000001,0},
    {"subnormal_input_mul_to_normal",2,0x00000001,0x7e800000,0},
    {"normal_mul_produces_subnormal",2,0x00800000,0x3f000000,0},
    {"negative_normal_mul_underflows",2,0x80800000,0x3f000000,0},
    {"sub_normal_minus_subnormal",1,0x00800000,0x007fffff,0},
    {"fma_subnormal_input",3,0x00000001,0x7e800000,0},
    {"fma_subnormal_output",3,0x00800000,0x3f000000,0},
    {"fma_cancellation",3,0x3f800001,0x3f7ffffe,0xbf800000},
    {"mul_positive_zero_negative_one",2,0,0xbf800000,0},
    {"mul_negative_zero_positive_one",2,0x80000000,0x3f800000,0},
    {"add_negative_zeros",0,0x80000000,0x80000000,0},
    {"add_opposite_zeros",0,0x80000000,0,0},
    {"fma_negative_zero",3,0x80000000,0x3f800000,0x80000000},
    {"min_negative_positive_zero",4,0x80000000,0,0},
    {"min_positive_negative_zero",4,0,0x80000000,0},
    {"max_negative_positive_zero",5,0x80000000,0,0},
    {"max_positive_negative_zero",5,0,0x80000000,0},
    {"min_subnormal_normal",4,1,0x00800000,0},
    {"max_negative_subnormal_negative_normal",5,0x80000001,0x80800000,0},
    {"int_from_subnormal",6,1,0,0},
    {"int_from_negative_subnormal",6,0x80000001,0,0},
    {"int_from_1point9",6,0x3ff33333,0,0},
    {"float_from_int_rounding",7,16777219,0,0},
    {"half_subnormal_pack",8,0x33800000,0,0},
    {"half_negative_zero_pack",8,0x80000000,0,0},
    {"half_pack_rounding_positive",8,0x3f803000,0,0},
    {"half_pack_rounding_negative",8,0xbf803000,0,0},
    {"half_subnormal_unpack",9,1,0,0},
    {"half_negative_zero_unpack",9,0x8000,0,0},
    {"compare_subnormal_zero",10,1,0,0},
    {"compare_negative_subnormal_zero",10,0x80000001,0,0},
    {"divide_by_negative_zero",11,0x3f800000,0x80000000,0},
    {"min_nan_number",4,0x7fc12345,0x3f800000,0},
    {"max_number_nan",5,0x3f800000,0x7fc12345,0},
    {"mul_zero_inf",2,0,0x7f800000,0},
    {"add_inf_negative_inf",0,0x7f800000,0xff800000,0},
    {"negate_negative_zero",12,0x80000000,0,0},
    {"negate_subnormal",12,1,0,0},
    {"candidate_mul_positive_zero_negative_one",13,0,0xbf800000,0},
    {"candidate_mul_negative_zero_positive_one",13,0x80000000,0x3f800000,0},
    {"candidate_mul_negative_denormal",13,0x80000001,0x3f800000,0},
    {"candidate_mul_underflow",13,0x80800000,0x3f000000,0},
    {"candidate_mul_zero_inf",13,0,0x7f800000,0},
    {"nonfused_mul_add_cancellation",14,0x3f800001,0x3f7ffffe,0xbf800000},
    {"candidate_fma_positive_zero_negative_one",15,0,0xbf800000,0},
    {"candidate_fma_negative_zero_positive_one",15,0x80000000,0x3f800000,0},
    {"candidate_fma_negative_denormal",15,0x80000001,0x3f800000,0},
    {"candidate_fma_underflow",15,0x80800000,0x3f000000,0},
    {"candidate_fma_zero_inf",15,0,0x7f800000,0},
    {"mul_negative_denormal",2,0x80000001,0x3f800000,0},
    {"mul_negative_finite",2,0xbfc00000,0x40000000,0},
    {"mul_overflow_positive",2,0x7f7fffff,0x40000000,0},
    {"mul_overflow_negative",2,0xff7fffff,0x40000000,0},
    {"mul_negative_inf",2,0x7f800000,0xbf800000,0},
    {"mul_zero_negative_inf",2,0,0xff800000,0},
    {"mul_quiet_nan",2,0x7fc12345,0x3f800000,0},
    {"mul_positive_zeros",2,0,0x3f800000,0},
    {"mul_double_negative_zero",2,0x80000000,0xbf800000,0},
    {"fma_negative_zero_positive_addend",3,0x80000000,0x3f800000,0},
};
#endif
int main(int argc,char **argv) {
#if defined(BB_FP16_CASES) || defined(BB_PKRTZ_CASES)
    assert(argc==3);
    const int mode=atoi(argv[2]); assert(mode>=0 && mode<4);
    const int benchmark=0;
#ifdef BB_PKRTZ_CASES
    make_cases();
#endif
#else
    assert(argc==2 || (argc==3 && !strcmp(argv[2],"--benchmark")));
    const int benchmark=argc==3;
#endif
    test_alarm(45); setbuf(stdout,NULL); puts("float modes probe: start");
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.pApplicationName="Float modes probe",.apiVersion=VK_API_VERSION_1_3};
    VkInstanceCreateInfo ic={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};
#ifdef __APPLE__
    const char *ie[]={VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME};
    ic.flags=VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    ic.enabledExtensionCount=1; ic.ppEnabledExtensionNames=ie;
#endif
    VkInstance instance; CHECK(vkCreateInstance(&ic,NULL,&instance));
    uint32_t count=1; CHECK(vkEnumeratePhysicalDevices(instance,&count,&physical)); assert(count);
    VkPhysicalDeviceFloatControlsProperties fc={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES};
    VkPhysicalDeviceProperties2 props={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,.pNext=&fc};
    vkGetPhysicalDeviceProperties2(physical,&props);
    printf("device=%s vendor=%x driver=%u\n",props.properties.deviceName,props.properties.vendorID,props.properties.driverVersion);
    printf("independence denorm=%u rounding=%u\n",fc.denormBehaviorIndependence,fc.roundingModeIndependence);
#define PRINT_WIDTH(w) printf("float" #w " preserve=%u flush=%u signed_zero_inf_nan=%u RTE=%u RTZ=%u\n",fc.shaderDenormPreserveFloat##w,fc.shaderDenormFlushToZeroFloat##w,fc.shaderSignedZeroInfNanPreserveFloat##w,fc.shaderRoundingModeRTEFloat##w,fc.shaderRoundingModeRTZFloat##w)
    PRINT_WIDTH(16); PRINT_WIDTH(32); PRINT_WIDTH(64);
    VkPhysicalDeviceVulkan12Properties v12={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
    props.pNext=&v12; vkGetPhysicalDeviceProperties2(physical,&props);
    printf("Vk12 samequery float32 preserve=%u flush=%u signed_zero_inf_nan=%u RTE=%u RTZ=%u\n",v12.shaderDenormPreserveFloat32,v12.shaderDenormFlushToZeroFloat32,v12.shaderSignedZeroInfNanPreserveFloat32,v12.shaderRoundingModeRTEFloat32,v12.shaderRoundingModeRTZFloat32);
    VkPhysicalDeviceVulkan12Features f12={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 f2={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,.pNext=&f12}; vkGetPhysicalDeviceFeatures2(physical,&f2);
    printf("features BDA=%u shaderInt64=%u shaderFloat16=%u shaderFloat64=%u\n",f12.bufferDeviceAddress,f2.features.shaderInt64,f12.shaderFloat16,f2.features.shaderFloat64); assert(f12.bufferDeviceAddress && f2.features.shaderInt64);
    f12=(VkPhysicalDeviceVulkan12Features){.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,.bufferDeviceAddress=VK_TRUE,.shaderInt8=VK_TRUE};
#if defined(BB_FP16_CASES) || defined(BB_PKRTZ_CASES)
    f12.shaderFloat16=VK_TRUE;
#endif
    VkQueueFamilyProperties families[32]; count=32; vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,families);
    uint32_t family=0; while (family<count && !(families[family].queueFlags&VK_QUEUE_COMPUTE_BIT)) ++family; assert(family<count);
    float priority=1; VkDeviceQueueCreateInfo qi={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=family,.queueCount=1,.pQueuePriorities=&priority};
    VkPhysicalDeviceFeatures features={.shaderInt64=VK_TRUE,.shaderInt16=VK_TRUE};
    VkDeviceCreateInfo dc={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.pNext=&f12,.queueCreateInfoCount=1,.pQueueCreateInfos=&qi,
        .pEnabledFeatures=&features};
#ifdef __APPLE__
    const char *extensions[]={"VK_KHR_portability_subset"};
    dc.enabledExtensionCount=1; dc.ppEnabledExtensionNames=extensions;
#endif
    CHECK(vkCreateDevice(physical,&dc,NULL,&device));
    vkGetDeviceQueue(device,family,0,&queue); vkGetPhysicalDeviceMemoryProperties(physical,&memory_properties);
    VkCommandPoolCreateInfo cp={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=family}; CHECK(vkCreateCommandPool(device,&cp,NULL,&commands));
    FILE *f=fopen(argv[1],"rb"); assert(f); assert(!fseek(f,0,SEEK_END)); long bytes=ftell(f); rewind(f);
    uint32_t *code=malloc((size_t)bytes); assert(code && fread(code,1,bytes,f)==(size_t)bytes); fclose(f);
    VkShaderModuleCreateInfo sm={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=(size_t)bytes,.pCode=code};
    VkShaderModule shader; CHECK(vkCreateShaderModule(device,&sm,NULL,&shader)); free(code);
    VkPushConstantRange range={.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT,.size=8};
    VkPipelineLayoutCreateInfo pl={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.pushConstantRangeCount=1,.pPushConstantRanges=&range}; CHECK(vkCreatePipelineLayout(device,&pl,NULL,&layout));
    VkComputePipelineCreateInfo pc={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,.stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=shader,.pName="main"},.layout=layout};
    CHECK(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&pc,NULL,&pipeline));

#ifdef BB_PKRTZ_CASES
    const size_t n=case_count, bytes_data=n*8*sizeof(uint32_t);
#else
    const size_t n=benchmark ? 65536 : sizeof(cases)/sizeof(*cases), bytes_data=n*8*sizeof(uint32_t);
#endif
    VkBufferCreateInfo bi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=bytes_data,
        .usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer buffer; CHECK(vkCreateBuffer(device,&bi,NULL,&buffer));
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(device,buffer,&req);
    unsigned type=0;
    for(;type<memory_properties.memoryTypeCount;++type)
        if((req.memoryTypeBits&(1u<<type)) && (memory_properties.memoryTypes[type].propertyFlags&6)==6) break;
    assert(type<memory_properties.memoryTypeCount);
    VkMemoryAllocateFlagsInfo flags={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,.flags=VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    VkMemoryAllocateInfo mi={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.pNext=&flags,.allocationSize=req.size,.memoryTypeIndex=type};
    VkDeviceMemory memory; CHECK(vkAllocateMemory(device,&mi,NULL,&memory)); CHECK(vkBindBufferMemory(device,buffer,memory,0));
    uint32_t *words; CHECK(vkMapMemory(device,memory,0,req.size,0,(void **)&words));
    memset(words,0,bytes_data);
    for(size_t i=0;i<n;++i) {
        words[8*i]=benchmark ? 2 : cases[i].op;
        words[8*i+1]=benchmark ? (0x3f800000u+((i&7)<<19)) | ((i&1)?0x80000000u:0) : cases[i].a;
        words[8*i+2]=benchmark ? 0x3f800001u | ((i&2)?0x80000000u:0) : cases[i].b;
        words[8*i+3]=benchmark ? 0 : cases[i].c;
        words[8*i+4]=0xdeadbeef;
#if defined(BB_FP16_CASES) || defined(BB_PKRTZ_CASES)
        words[8*i+5]=0xcdcdcdcd;
        words[8*i+6]=0xa5a5a5a5;
        words[8*i+7]=0x5a5a5a5a;
#endif
    }
    VkQueryPool queries=VK_NULL_HANDLE;
    if(benchmark) {
        assert(props.properties.limits.timestampComputeAndGraphics && families[family].timestampValidBits);
        VkQueryPoolCreateInfo qci={.sType=VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,.queryType=VK_QUERY_TYPE_TIMESTAMP,.queryCount=2};
        CHECK(vkCreateQueryPool(device,&qci,NULL,&queries));
    }
    VkBufferDeviceAddressInfo address_info={.sType=VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,.buffer=buffer};
    uint64_t address=vkGetBufferDeviceAddress(device,&address_info); assert(address);
    VkCommandBufferAllocateInfo cai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=commands,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    VkCommandBuffer cmd; CHECK(vkAllocateCommandBuffers(device,&cai,&cmd));
    VkCommandBufferBeginInfo cbi={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; CHECK(vkBeginCommandBuffer(cmd,&cbi));
    VkMemoryBarrier before={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT,.dstAccessMask=VK_ACCESS_SHADER_READ_BIT};
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&before,0,NULL,0,NULL);
    if(benchmark) {
        vkCmdResetQueryPool(cmd,queries,0,2);
        vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,queries,0);
    }
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
    vkCmdPushConstants(cmd,layout,VK_SHADER_STAGE_COMPUTE_BIT,0,8,&address); vkCmdDispatch(cmd,(uint32_t)n/(benchmark?64:1),1,1);
    if(benchmark) vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,queries,1);
    VkMemoryBarrier after={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&after,0,NULL,0,NULL);
    CHECK(vkEndCommandBuffer(cmd));
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&cmd};
    for(int repetition=0;repetition<(benchmark?24:1);++repetition) {
        CHECK(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE)); CHECK(vkQueueWaitIdle(queue));
        if(benchmark) {
            uint64_t ticks[2];
            CHECK(vkGetQueryPoolResults(device,queries,0,2,sizeof(ticks),ticks,sizeof(uint64_t),VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WAIT_BIT));
            const unsigned valid=families[family].timestampValidBits;
            const uint64_t mask=valid==64 ? UINT64_MAX : (UINT64_C(1)<<valid)-1;
            printf("GPU_SAMPLE %s %d %.6f ms\n",argv[1],repetition,
                   ((ticks[1]-ticks[0])&mask)*props.properties.limits.timestampPeriod/1e6);
        }
    }
    unsigned differences=0;
    for(size_t i=0;i<n;++i) {
        if(benchmark) {
            float initial, multiplier;
            memcpy(&initial,words+8*i+1,4); memcpy(&multiplier,words+8*i+2,4);
            volatile float product=initial;
            for(unsigned j=0;j<128;++j) product=product*multiplier;
            float expected_float=product; uint32_t expected;
            memcpy(&expected,&expected_float,4);
            assert(words[8*i+4]==expected);
            continue;
        }
        uint32_t actual=words[8*i+4]; assert(actual!=0xdeadbeef);
#ifndef BB_PKRTZ_CASES
        printf("%-44s %08x\n",cases[i].name,actual);
#endif
#if defined(BB_FP16_CASES) || defined(BB_PKRTZ_CASES)
        const Case *c=cases+i;
#ifdef BB_PKRTZ_CASES
        const uint32_t low=reference_half(c->a,mode), high=reference_half(c->b,mode);
        const uint32_t expected=low | (high<<16);
        const int match=half_matches(actual&0xffffu,low) && half_matches(actual>>16,high);
        if(!match) {
            if(differences<16) printf("MISMATCH mode=%d input=%08x/%08x expected=%08x actual=%08x\n",mode,c->a,c->b,expected,actual);
            ++differences;
        }
#else
        uint32_t expected=c->c;
        if(c->op==0 && (mode==0 || mode==1) && (expected&0x7fffu)>0 && (expected&0x7fffu)<0x400u)
            expected&=0x8000u;
        if(c->op==1 && (mode==0 || mode==2) && (c->a&0x7fffu)>0 && (c->a&0x7fffu)<0x400u)
            expected=(c->a&0x8000u)<<16;
        const uint32_t magnitude=c->op==0 ? 0x7fffu : 0x7fffffffu;
        const uint32_t infinity=c->op==0 ? 0x7c00u : 0x7f800000u;
        const int match=c->op!=2 && (expected&magnitude)>infinity
            ? (actual&magnitude)>infinity : actual==expected;
        if(!match) { printf("MISMATCH mode=%d op=%u input=%08x expected=%08x actual=%08x\n",mode,c->op,c->a,expected,actual); ++differences; }
#endif
        assert(words[8*i]==c->op && words[8*i+1]==c->a && words[8*i+2]==c->b && words[8*i+3]==c->c);
        assert(words[8*i+5]==0xcdcdcdcd && words[8*i+6]==0xa5a5a5a5 && words[8*i+7]==0x5a5a5a5a);
#else
        if ((!strcmp(cases[i].name,"mul_positive_zero_negative_one") ||
             !strcmp(cases[i].name,"mul_negative_zero_positive_one")) && actual!=0x80000000) ++differences;
        if (!strcmp(cases[i].name,"nonfused_mul_add_cancellation") && actual!=0) ++differences;
        if(cases[i].op==13 || cases[i].op==15) {
            if(strstr(cases[i].name,"zero_inf")) assert((actual&0x7fffffff)>0x7f800000);
            else assert(actual==0x80000000);
        }
#endif
    }
    vkUnmapMemory(device,memory); vkDestroyBuffer(device,buffer,NULL); vkFreeMemory(device,memory,NULL);
    vkDestroyPipeline(device,pipeline,NULL); vkDestroyPipelineLayout(device,layout,NULL); vkDestroyShaderModule(device,shader,NULL);
    if(queries) vkDestroyQueryPool(device,queries,NULL);
    vkDestroyCommandPool(device,commands,NULL); vkDestroyDevice(device,NULL); vkDestroyInstance(instance,NULL);
    if(benchmark) { puts("BENCHMARK: 8,388,608 production multiplies; 65,536 CPU/GPU bitwise results PASS"); return 0; }
#if defined(BB_FP16_CASES) || defined(BB_PKRTZ_CASES)
#ifdef BB_PKRTZ_CASES
    printf("MEASURED: PKRTZ mode=%d; %zu packed pairs; differences=%u; input/tail canaries PASS\n",mode,n,differences);
#else
    printf("MEASURED: OpFConvert mode=%d; %zu cases; differences=%u; input/tail canaries PASS\n",mode,n,differences);
#endif
#else
    printf("MEASURED: %zu cases; signed-zero/nonfused semantic differences=%u; candidate sign repair passed\n",n,differences);
#endif
    return differences ? 2 : 0;
}
