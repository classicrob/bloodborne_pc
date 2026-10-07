/* Standalone MoltenVK host import over the actual Mach backend. */
#include "../../src/runtime_memory.c"
#include <assert.h>
#include <signal.h>
#include <vulkan/vulkan.h>
#include <sys/ucontext.h>
#include <sys/resource.h>
uintptr_t runtime_lookup(const RuntimeExport *table,size_t count,const char *name) {
    for (size_t i=0;i<count;++i) if (!strcmp(table[i].name,name)) return (uintptr_t)table[i].function;
    return 0;
}
#define CHECK(call) do { VkResult r_=(call); if (r_!=VK_SUCCESS) { fprintf(stderr,"FAIL %s: %d\n",#call,r_); exit(1); } } while (0)
static VkDevice device;
static VkPhysicalDevice physical;
static VkQueue queue;
static VkCommandPool commands;
static VkPipeline pipeline;
static VkPipelineLayout layout;
static PFN_vkGetMemoryHostPointerPropertiesEXT host_properties;
static VkPhysicalDeviceMemoryProperties memory_properties;
static VkDeviceSize import_alignment;
typedef struct { VkBuffer buffer; VkDeviceMemory memory; VkDeviceAddress address; VkDeviceSize size; } Imported;
static volatile sig_atomic_t faults;
static uintptr_t protected_start;
static size_t protected_size;
static void write_fault(int sig,siginfo_t *info,void *context) {
    ucontext_t *uc=context;
    uintptr_t at=(uintptr_t)info->si_addr;
    if ((sig!=SIGBUS && sig!=SIGSEGV) || at<protected_start || at>=protected_start+protected_size ||
        !(uc->uc_mcontext->__es.__err & 2)) _exit(99);
    ++faults;
    if (mprotect((void *)protected_start,protected_size,PROT_READ|PROT_WRITE)) _exit(98);
}
static Imported import(void *pointer,VkDeviceSize size,const char *name) {
    struct timespec started,finished; clock_gettime(CLOCK_MONOTONIC,&started);
    Imported out={.size=size};
    VkMemoryHostPointerPropertiesEXT hp={.sType=VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    VkResult r=host_properties(device,VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,pointer,&hp);
    printf("import %s pointer=%p bytes=%llu query=%d types=%x\n",name,pointer,(unsigned long long)size,r,hp.memoryTypeBits);
    if (r) return out;
    VkExternalMemoryBufferCreateInfo external={.sType=VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    VkBufferCreateInfo ci={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.pNext=&external,.size=size,
        .usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    r=vkCreateBuffer(device,&ci,NULL,&out.buffer);
    if (r) { printf("create buffer: %d\n",r); return out; }
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(device,out.buffer,&req);
    uint32_t type=UINT32_MAX;
    for (uint32_t i=0;i<memory_properties.memoryTypeCount;++i)
        if (((hp.memoryTypeBits&req.memoryTypeBits)>>i)&1u)
            if ((memory_properties.memoryTypes[i].propertyFlags&(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))==
                (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { type=i; break; }
    printf("requirements size=%llu alignment=%llu types=%x selected=%u flags=%x\n",
        (unsigned long long)req.size,(unsigned long long)req.alignment,req.memoryTypeBits,type,
        type==UINT32_MAX ? 0 : memory_properties.memoryTypes[type].propertyFlags);
    assert(type!=UINT32_MAX && req.size<=size && size%import_alignment==0 && (uintptr_t)pointer%import_alignment==0);
    VkImportMemoryHostPointerInfoEXT host={.sType=VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
        .handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,.pHostPointer=pointer};
    VkMemoryAllocateFlagsInfo flags={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,.pNext=&host,
        .flags=VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    VkMemoryAllocateInfo alloc={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.pNext=&flags,.allocationSize=size,.memoryTypeIndex=type};
    r=vkAllocateMemory(device,&alloc,NULL,&out.memory); printf("allocate=%d\n",r);
    if (r) { vkDestroyBuffer(device,out.buffer,NULL); out.buffer=VK_NULL_HANDLE; return out; }
    CHECK(vkBindBufferMemory(device,out.buffer,out.memory,0));
    VkBufferDeviceAddressInfo ai={.sType=VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,.buffer=out.buffer};
    out.address=vkGetBufferDeviceAddress(device,&ai);
    clock_gettime(CLOCK_MONOTONIC,&finished);
    struct rusage usage; assert(!getrusage(RUSAGE_SELF,&usage));
    double ms=(finished.tv_sec-started.tv_sec)*1000.0+(finished.tv_nsec-started.tv_nsec)/1e6;
    printf("BDA=%#llx import_ms=%.3f maxrss_MiB=%.1f\n",(unsigned long long)out.address,ms,usage.ru_maxrss/1048576.0); assert(out.address);
    return out;
}
static void destroy(Imported *in) {
    CHECK(vkDeviceWaitIdle(device)); vkDestroyBuffer(device,in->buffer,NULL); vkFreeMemory(device,in->memory,NULL); *in=(Imported){0};
}
static void dispatch(const Imported *in,uint32_t index,uint32_t addend) {
    VkCommandBufferAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=commands,
        .level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    VkCommandBuffer cmd; CHECK(vkAllocateCommandBuffers(device,&ai,&cmd));
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; CHECK(vkBeginCommandBuffer(cmd,&begin));
    VkMemoryBarrier before={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT,.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT};
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&before,0,NULL,0,NULL);
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
    struct { uint64_t address; uint32_t index,addend; } push={in->address,index,addend};
    vkCmdPushConstants(cmd,layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),&push);
    vkCmdDispatch(cmd,1,1,1);
    VkMemoryBarrier after={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&after,0,NULL,0,NULL);
    CHECK(vkEndCommandBuffer(cmd));
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&cmd};
    CHECK(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE)); CHECK(vkQueueWaitIdle(queue));
    vkFreeCommandBuffers(device,commands,1,&cmd);
}
int main(int argc,char **argv) {
    assert(argc==2); setbuf(stdout,NULL); puts("external host probe: start");
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.pApplicationName="Mach external host probe",.apiVersion=VK_API_VERSION_1_3};
    const char *ie[]={VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME};
    VkInstanceCreateInfo ic={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.flags=VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR,
        .pApplicationInfo=&app,.enabledExtensionCount=1,.ppEnabledExtensionNames=ie};
    VkInstance instance; CHECK(vkCreateInstance(&ic,NULL,&instance));
    uint32_t count=1; CHECK(vkEnumeratePhysicalDevices(instance,&count,&physical)); assert(count);
    VkPhysicalDeviceMaintenance4Properties m4={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_PROPERTIES};
    VkPhysicalDeviceMaintenance3Properties m3={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES,.pNext=&m4};
    VkPhysicalDeviceExternalMemoryHostPropertiesEXT hp={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT,.pNext=&m3};
    VkPhysicalDeviceProperties2 props={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,.pNext=&hp};
    vkGetPhysicalDeviceProperties2(physical,&props); import_alignment=hp.minImportedHostPointerAlignment;
    printf("device=%s import_alignment=%llu max_allocation=%llu max_buffer=%llu\n",props.properties.deviceName,
        (unsigned long long)import_alignment,(unsigned long long)m3.maxMemoryAllocationSize,(unsigned long long)m4.maxBufferSize);
    VkPhysicalDeviceExternalBufferInfo ei={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO,
        .usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    VkExternalBufferProperties ep={.sType=VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES}; vkGetPhysicalDeviceExternalBufferProperties(physical,&ei,&ep);
    printf("external features=%x compatible=%x\n",ep.externalMemoryProperties.externalMemoryFeatures,ep.externalMemoryProperties.compatibleHandleTypes);
    assert(ep.externalMemoryProperties.externalMemoryFeatures&VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT);
    VkPhysicalDeviceVulkan12Features f12={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 f2={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,.pNext=&f12}; vkGetPhysicalDeviceFeatures2(physical,&f2);
    printf("features BDA=%u shaderInt64=%u\n",f12.bufferDeviceAddress,f2.features.shaderInt64); assert(f12.bufferDeviceAddress && f2.features.shaderInt64);
    f12=(VkPhysicalDeviceVulkan12Features){.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,.bufferDeviceAddress=VK_TRUE};
    VkQueueFamilyProperties families[32]; count=32; vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,families);
    uint32_t family=0; while (family<count && !(families[family].queueFlags&VK_QUEUE_COMPUTE_BIT)) ++family; assert(family<count);
    float priority=1; VkDeviceQueueCreateInfo qi={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=family,.queueCount=1,.pQueuePriorities=&priority};
    const char *extensions[]={"VK_KHR_portability_subset",VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME};
    VkPhysicalDeviceFeatures features={.shaderInt64=VK_TRUE};
    VkDeviceCreateInfo dc={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.pNext=&f12,.queueCreateInfoCount=1,.pQueueCreateInfos=&qi,
        .enabledExtensionCount=2,.ppEnabledExtensionNames=extensions,.pEnabledFeatures=&features}; CHECK(vkCreateDevice(physical,&dc,NULL,&device));
    vkGetDeviceQueue(device,family,0,&queue); vkGetPhysicalDeviceMemoryProperties(physical,&memory_properties);
    host_properties=(PFN_vkGetMemoryHostPointerPropertiesEXT)vkGetDeviceProcAddr(device,"vkGetMemoryHostPointerPropertiesEXT"); assert(host_properties);
    VkCommandPoolCreateInfo cp={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=family}; CHECK(vkCreateCommandPool(device,&cp,NULL,&commands));
    FILE *f=fopen(argv[1],"rb"); assert(f); assert(!fseek(f,0,SEEK_END)); long bytes=ftell(f); rewind(f);
    uint32_t *code=malloc((size_t)bytes); assert(code && fread(code,1,bytes,f)==(size_t)bytes); fclose(f);
    VkShaderModuleCreateInfo sm={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=(size_t)bytes,.pCode=code};
    VkShaderModule shader; CHECK(vkCreateShaderModule(device,&sm,NULL,&shader)); free(code);
    VkPushConstantRange range={.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT,.size=16};
    VkPipelineLayoutCreateInfo pl={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.pushConstantRangeCount=1,.pPushConstantRanges=&range}; CHECK(vkCreatePipelineLayout(device,&pl,NULL,&layout));
    VkComputePipelineCreateInfo pc={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,.stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=shader,.pName="main"},.layout=layout};
    CHECK(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&pc,NULL,&pipeline));
    int64_t phys; assert(!direct_allocate(0,POOL_SIZE,4*PAGE,PAGE,0,&phys));
    void *a=NULL,*b=NULL; assert(!direct_map(&a,4*PAGE,3,0,phys,PAGE)); assert(!direct_map(&b,4*PAGE,3,0,phys,PAGE));
    uint32_t *back=(uint32_t *)(backing_base+phys), *alias=a, *other=b;
    struct sigaction sa={.sa_sigaction=write_fault,.sa_flags=SA_SIGINFO}; sigemptyset(&sa.sa_mask); assert(!sigaction(SIGBUS,&sa,NULL)); assert(!sigaction(SIGSEGV,&sa,NULL));
    for (int view=0;view<2;++view) {
        Imported in=import(view ? a : back,4*PAGE,view ? "guest-alias" : "backing"); assert(in.memory);
        other[1]=100; dispatch(&in,0,23); assert(back[0]==123 && alias[0]==123 && other[0]==123);
        printf("PASS %s bidirectional alias coherence\n",view ? "guest" : "backing");
        protected_start=(uintptr_t)a; protected_size=4*PAGE;
        assert(!mprotect(a,4*PAGE,PROT_READ)); other[1]=200;
        dispatch(&in,0,34); assert(alias[0]==234 && back[0]==234 && !faults);
        ((volatile uint32_t *)a)[1]=300; assert(faults==1); faults=0;
        dispatch(&in,0,45); assert(other[0]==345);
        printf("PASS GPU write with guest alias RO; CPU store faults and resumes\n");
        destroy(&in);
    }
    Imported sub=import((unsigned char *)a+4096,4096,"minimum-aligned-subview"); assert(sub.memory);
    other[4096/4+1]=600; dispatch(&sub,0,89); assert(back[4096/4]==689); destroy(&sub);
    Imported one=import(a,4*PAGE,"simultaneous-alias-one"), two=import(b,4*PAGE,"simultaneous-alias-two");
    assert(one.memory && two.memory);
    alias[1]=700; dispatch(&one,0,90); assert(other[0]==790);
    other[1]=800; dispatch(&two,0,91); assert(alias[0]==891);
    destroy(&one); destroy(&two); puts("PASS minimum4KiB subview and simultaneous imports of aliases");
    /* A virtually contiguous range composed from noncontiguous physical offsets. */
    void *joined=(void *)(USER_MIN+UINT64_C(0x10000000));
    assert(!direct_map(&joined,PAGE,3,MAP_FIXED_FLAG,phys,PAGE));
    void *second=(unsigned char *)joined+PAGE; assert(!direct_map(&second,PAGE,3,MAP_FIXED_FLAG,phys+2*PAGE,PAGE));
    Imported fragmented=import(joined,2*PAGE,"noncontiguous-physical-pages"); assert(fragmented.memory);
    back[1]=400; back[2*PAGE/4+1]=500;
    dispatch(&fragmented,0,56); dispatch(&fragmented,PAGE/4,67);
    assert(back[0]==456 && back[2*PAGE/4]==567); puts("PASS import spans distinct Mach remaps"); destroy(&fragmented);
    /* Vulkan lifetime ends before replacing the backing, as required by the spec. */
    assert(!direct_release(phys,4*PAGE));
    int64_t reused; assert(!direct_allocate(phys,phys+4*PAGE,4*PAGE,PAGE,0,&reused) && reused==phys);
    a=NULL; assert(!direct_map(&a,4*PAGE,3,0,phys,PAGE));
    back=(uint32_t *)(backing_base+phys); for (unsigned i=0;i<4*PAGE/4;++i) assert(!back[i]);
    Imported fresh=import(a,4*PAGE,"fresh-backing-after-release"); assert(fresh.memory);
    dispatch(&fresh,0,78); assert(back[0]==78); destroy(&fresh); assert(!direct_release(phys,4*PAGE));
    puts("PASS destroy-import -> release/zero -> remap/reimport -> BDA execution");
    const unsigned sizes[]={1,16,256,4096,8192};
    for (unsigned si=0;si<sizeof(sizes)/sizeof(*sizes);++si) {
        unsigned mib=sizes[si];
        size_t size=(size_t)mib<<20; void *p=mmap(NULL,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0); assert(p!=MAP_FAILED);
        Imported large=import(p,size,"size-survey"); if (!large.memory) { munmap(p,size); break; }
        uint32_t last=(uint32_t)(size/4-2); ((uint32_t *)p)[last+1]=mib;
        dispatch(&large,last,99); assert(((uint32_t *)p)[last]==mib+99); destroy(&large); assert(!munmap(p,size));
    }
    CHECK(vkDeviceWaitIdle(device)); vkDestroyPipeline(device,pipeline,NULL); vkDestroyPipelineLayout(device,layout,NULL);
    vkDestroyShaderModule(device,shader,NULL); vkDestroyCommandPool(device,commands,NULL); vkDestroyDevice(device,NULL); vkDestroyInstance(instance,NULL);
    puts("PASS external host probe complete");
}
