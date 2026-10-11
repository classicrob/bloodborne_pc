// SPDX-License-Identifier: GPL-2.0-or-later
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>
#define VK_USE_PLATFORM_METAL_EXT
#include <vulkan/vulkan.h>
#include "ffx_vk_portable.h"
#include "../../gpu/shim/macos_metalfx.h"
#include <memory>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <chrono>
#include <thread>
#include <vector>
#include <sys/sysctl.h>

static constexpr uint32_t W=2560, H=1440, OW=3840, OH=2160;
static void require(bool ok, const char* feature) {
    if (!ok) throw std::runtime_error(feature);
}
static void vkcheck(VkResult r, const char* call) {
    if (r != VK_SUCCESS) throw std::runtime_error(std::string(call)+" VkResult="+std::to_string(r));
}
#define VK(call) vkcheck((call), #call)
struct Image {
    VkImage image{};
    VkDeviceMemory memory{};
    VkImageView view{};
    VkFormat format{};
    VkImageAspectFlags aspect{};
    uint32_t w{}, h{};
    VkImageUsageFlags usage{};
    id<MTLTexture> texture{};
};
struct Env {
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    VkQueue queue{};
    uint32_t family{};
    VkCommandPool pool{};
    PFN_vkExportMetalObjectsEXT exportMetal{};
    id<MTLDevice> metal{};
    id<MTLCommandQueue> metalQueue{};
    bool fp16{};
    VkPhysicalDeviceProperties properties{};
    Env() {
        VkExportMetalObjectCreateInfoEXT exportQueue{VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECT_CREATE_INFO_EXT};
        exportQueue.exportObjectType=VK_EXPORT_METAL_OBJECT_TYPE_METAL_COMMAND_QUEUE_BIT_EXT;
        VkExportMetalObjectCreateInfoEXT exportDevice{VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECT_CREATE_INFO_EXT};
        exportDevice.exportObjectType=VK_EXPORT_METAL_OBJECT_TYPE_METAL_DEVICE_BIT_EXT;
        exportDevice.pNext=&exportQueue;
        const char* ie[]={VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME,
            VK_KHR_SURFACE_EXTENSION_NAME,VK_EXT_METAL_SURFACE_EXTENSION_NAME};
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName="bbport standalone MetalFX";
        app.apiVersion=VK_API_VERSION_1_3;
        VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ci.pNext=&exportDevice; ci.pApplicationInfo=&app;
        ci.flags=VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        ci.enabledExtensionCount=3; ci.ppEnabledExtensionNames=ie;
        VK(vkCreateInstance(&ci,nullptr,&instance));
        uint32_t count=0; VK(vkEnumeratePhysicalDevices(instance,&count,nullptr));
        require(count==1,"prototype requires exactly one selected ICD device");
        VK(vkEnumeratePhysicalDevices(instance,&count,&physical));
        vkGetPhysicalDeviceProperties(physical,&properties);
        std::printf("Vulkan device=%s API=%u.%u driver=%u\n",properties.deviceName,
                    VK_API_VERSION_MAJOR(properties.apiVersion),VK_API_VERSION_MINOR(properties.apiVersion),
                    properties.driverVersion);
        VK(vkEnumerateDeviceExtensionProperties(physical,nullptr,&count,nullptr));
        std::vector<VkExtensionProperties> extensions(count);
        VK(vkEnumerateDeviceExtensionProperties(physical,nullptr,&count,extensions.data()));
        auto has=[&](const char* name) {
            return std::any_of(extensions.begin(),extensions.end(),[&](auto& x) {
                return !std::strcmp(x.extensionName,name);});
        };
        require(has(VK_EXT_METAL_OBJECTS_EXTENSION_NAME),"VK_EXT_metal_objects required");
        const char* de[]={VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME,VK_EXT_METAL_OBJECTS_EXTENSION_NAME,
            VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,nullptr);
        std::vector<VkQueueFamilyProperties> queues(count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,queues.data());
        while(family<count && (queues[family].queueFlags & (VK_QUEUE_COMPUTE_BIT|VK_QUEUE_GRAPHICS_BIT)) !=
              (VK_QUEUE_COMPUTE_BIT|VK_QUEUE_GRAPHICS_BIT)) ++family;
        require(family<count,"graphics+compute queue required");
        VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        f12.pNext=&f13;
        VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2}; f2.pNext=&f12;
        vkGetPhysicalDeviceFeatures2(physical,&f2);
        require(f12.timelineSemaphore && f13.synchronization2,"timeline+sync2 required");
        require(f2.features.shaderStorageImageWriteWithoutFormat,"FSR storage image writes required");
        fp16=f12.shaderFloat16;
        VkPhysicalDeviceVulkan13Features enable13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        enable13.synchronization2=VK_TRUE;
        VkPhysicalDeviceVulkan12Features enable12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        enable12.pNext=&enable13; enable12.timelineSemaphore=VK_TRUE; enable12.shaderFloat16=fp16;
        VkPhysicalDeviceFeatures features{};
        features.shaderStorageImageWriteWithoutFormat=VK_TRUE;
        features.shaderStorageImageExtendedFormats=f2.features.shaderStorageImageExtendedFormats;
        float priority=1;
        VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qi.queueFamilyIndex=family; qi.queueCount=1; qi.pQueuePriorities=&priority;
        VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        di.pNext=&enable12; di.pEnabledFeatures=&features;
        di.queueCreateInfoCount=1; di.pQueueCreateInfos=&qi;
        di.enabledExtensionCount=3; di.ppEnabledExtensionNames=de;
        VK(vkCreateDevice(physical,&di,nullptr,&device));
        vkGetDeviceQueue(device,family,0,&queue);
        exportMetal=(PFN_vkExportMetalObjectsEXT)vkGetDeviceProcAddr(device,"vkExportMetalObjectsEXT");
        require(exportMetal,"vkExportMetalObjectsEXT missing");
        VkExportMetalDeviceInfoEXT md{VK_STRUCTURE_TYPE_EXPORT_METAL_DEVICE_INFO_EXT};
        VkExportMetalCommandQueueInfoEXT mq{VK_STRUCTURE_TYPE_EXPORT_METAL_COMMAND_QUEUE_INFO_EXT};
        mq.queue=queue; md.pNext=&mq;
        VkExportMetalObjectsInfoEXT out{VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT};
        out.pNext=&md; exportMetal(device,&out);
        metal=md.mtlDevice;
        require(metal && mq.mtlCommandQueue,"exported Metal device/queue missing");
        metalQueue=[metal newCommandQueue];
        require(metalQueue,"separate Metal queue missing");
        std::printf("Exported Metal device=%s; separate command queue\n",metal.name.UTF8String);
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.queueFamilyIndex=family; pi.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        VK(vkCreateCommandPool(device,&pi,nullptr,&pool));
    }
    uint32_t memoryType(uint32_t bits,VkMemoryPropertyFlags flags) {
        VkPhysicalDeviceMemoryProperties p{}; vkGetPhysicalDeviceMemoryProperties(physical,&p);
        for(uint32_t i=0;i<p.memoryTypeCount;++i)
            if((bits&(1u<<i)) && (p.memoryTypes[i].propertyFlags&flags)==flags) return i;
        throw std::runtime_error("required memory type unavailable");
    }
    Image image(VkFormat format,uint32_t w,uint32_t h,VkImageAspectFlags aspect) {
        Image x{}; x.format=format; x.w=w; x.h=h; x.aspect=aspect;
        x.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        x.usage|=aspect==VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT :
            VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        VkExportMetalObjectCreateInfoEXT exp{VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECT_CREATE_INFO_EXT};
        exp.exportObjectType=VK_EXPORT_METAL_OBJECT_TYPE_METAL_TEXTURE_BIT_EXT;
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.pNext=&exp; ci.imageType=VK_IMAGE_TYPE_2D; ci.format=format;
        ci.extent={w,h,1}; ci.mipLevels=1; ci.arrayLayers=1;
        ci.samples=VK_SAMPLE_COUNT_1_BIT; ci.tiling=VK_IMAGE_TILING_OPTIMAL;
        ci.usage=x.usage; ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        VK(vkCreateImage(device,&ci,nullptr,&x.image));
        VkMemoryRequirements req{}; vkGetImageMemoryRequirements(device,x.image,&req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize=req.size; ai.memoryTypeIndex=memoryType(req.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VK(vkAllocateMemory(device,&ai,nullptr,&x.memory));
        VK(vkBindImageMemory(device,x.image,x.memory,0));
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image=x.image; vi.viewType=VK_IMAGE_VIEW_TYPE_2D; vi.format=format;
        vi.subresourceRange={aspect,0,1,0,1};
        VK(vkCreateImageView(device,&vi,nullptr,&x.view));
        VkExportMetalTextureInfoEXT mt{VK_STRUCTURE_TYPE_EXPORT_METAL_TEXTURE_INFO_EXT};
        mt.image=x.image; mt.plane=VK_IMAGE_ASPECT_PLANE_0_BIT;
        VkExportMetalObjectsInfoEXT ei{VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT};
        ei.pNext=&mt; exportMetal(device,&ei); x.texture=mt.mtlTexture;
        require(x.texture,"MTLTexture export failed");
        require(x.texture.device==metal,"MetalFX/Vulkan device mismatch");
        return x;
    }
    VkCommandBuffer command() {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool=pool; ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount=1;
        VkCommandBuffer c{}; VK(vkAllocateCommandBuffers(device,&ai,&c)); return c;
    }
};
static void begin(VkCommandBuffer c) {
    VK(vkResetCommandBuffer(c,0));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT; VK(vkBeginCommandBuffer(c,&bi));
}
static void barrier(VkCommandBuffer c,const Image& x,VkImageLayout old,VkImageLayout next,
                    uint32_t src=VK_QUEUE_FAMILY_IGNORED,uint32_t dst=VK_QUEUE_FAMILY_IGNORED) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask=old==VK_IMAGE_LAYOUT_UNDEFINED || src==VK_QUEUE_FAMILY_EXTERNAL ? 0 :
        VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask=dst==VK_QUEUE_FAMILY_EXTERNAL ? 0 : VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout=old; b.newLayout=next; b.srcQueueFamilyIndex=src; b.dstQueueFamilyIndex=dst;
    b.image=x.image; b.subresourceRange={x.aspect,0,1,0,1};
    vkCmdPipelineBarrier(c,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         0,0,nullptr,0,nullptr,1,&b);
}
static void submit(Env& e,VkCommandBuffer c,VkSemaphore wait,uint64_t w,
                   VkSemaphore signal,uint64_t s,VkFence fence) {
    VK(vkEndCommandBuffer(c));
    VkTimelineSemaphoreSubmitInfo time{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    time.waitSemaphoreValueCount=wait?1:0; time.pWaitSemaphoreValues=&w;
    time.signalSemaphoreValueCount=signal?1:0; time.pSignalSemaphoreValues=&s;
    VkPipelineStageFlags stage=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.pNext=&time;
    si.waitSemaphoreCount=wait?1:0; si.pWaitSemaphores=&wait; si.pWaitDstStageMask=&stage;
    si.commandBufferCount=1; si.pCommandBuffers=&c;
    si.signalSemaphoreCount=signal?1:0; si.pSignalSemaphores=&signal;
    VK(vkQueueSubmit(e.queue,1,&si,fence));
}
static void complete(Env& e,VkFence f) {
    VK(vkWaitForFences(e.device,1,&f,VK_TRUE,30'000'000'000ull));
    VK(vkResetFences(e.device,1,&f));
}
static std::pair<VkSemaphore,id<MTLSharedEvent>> event(Env& e) {
    VkExportMetalObjectCreateInfoEXT exp{VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECT_CREATE_INFO_EXT};
    exp.exportObjectType=VK_EXPORT_METAL_OBJECT_TYPE_METAL_SHARED_EVENT_BIT_EXT;
    VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO}; type.pNext=&exp;
    type.semaphoreType=VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo ci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; ci.pNext=&type;
    VkSemaphore semaphore{}; VK(vkCreateSemaphore(e.device,&ci,nullptr,&semaphore));
    VkExportMetalSharedEventInfoEXT me{VK_STRUCTURE_TYPE_EXPORT_METAL_SHARED_EVENT_INFO_EXT};
    me.semaphore=semaphore;
    VkExportMetalObjectsInfoEXT out{VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT}; out.pNext=&me;
    e.exportMetal(e.device,&out); require(me.mtlSharedEvent,"timeline MTLSharedEvent export failed");
    return {semaphore,me.mtlSharedEvent};
}
static void metalComplete(id<MTLCommandBuffer> c) {
    [c waitUntilCompleted];
    require(c.status==MTLCommandBufferStatusCompleted,c.error.description.UTF8String ?: "Metal command failed");
}
static void report(const char* label,std::vector<double> ms) {
    std::sort(ms.begin(),ms.end()); double sum=0; for(auto v:ms) sum+=v;
    std::printf("%s samples=%zu mean=%.4fms median=%.4fms p95=%.4fms min=%.4fms max=%.4fms\n",
       label,ms.size(),sum/ms.size(),ms[ms.size()/2],ms[ms.size()*95/100],ms.front(),ms.back());
}
static void present(Env& e,const Image& output,VkSemaphore scaled,uint64_t value) {
    require(SDL_Init(SDL_INIT_VIDEO),"SDL video init failed");
    SDL_Window* window=SDL_CreateWindow("Standalone MetalFX -> Vulkan presentation",640,360,SDL_WINDOW_VULKAN);
    require(window,"SDL Vulkan window failed");
    VkSurfaceKHR surface{};
    require(SDL_Vulkan_CreateSurface(window,e.instance,nullptr,&surface),"Metal Vulkan surface failed");
    VkBool32 supported{}; VK(vkGetPhysicalDeviceSurfaceSupportKHR(e.physical,e.family,surface,&supported));
    require(supported,"selected queue cannot present");
    VkSurfaceCapabilitiesKHR caps{}; VK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(e.physical,surface,&caps));
    uint32_t count{}; VK(vkGetPhysicalDeviceSurfaceFormatsKHR(e.physical,surface,&count,nullptr));
    std::vector<VkSurfaceFormatKHR> formats(count);
    VK(vkGetPhysicalDeviceSurfaceFormatsKHR(e.physical,surface,&count,formats.data()));
    auto format=std::find_if(formats.begin(),formats.end(),[](auto f) {
        return f.format==VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace==VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    });
    require(format!=formats.end(),"required BGRA8 SDR surface format missing");
    require(caps.supportedUsageFlags&VK_IMAGE_USAGE_TRANSFER_DST_BIT,"surface transfer destination unsupported");
    VkExtent2D extent=caps.currentExtent;
    if(extent.width==UINT32_MAX) {
        int w{},h{}; require(SDL_GetWindowSizeInPixels(window,&w,&h),"drawable size failed");
        extent={std::clamp((uint32_t)w,caps.minImageExtent.width,caps.maxImageExtent.width),
                std::clamp((uint32_t)h,caps.minImageExtent.height,caps.maxImageExtent.height)};
    }
    VkSwapchainCreateInfoKHR si{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    si.surface=surface; si.minImageCount=caps.minImageCount; si.imageFormat=format->format;
    si.imageColorSpace=format->colorSpace; si.imageExtent=extent; si.imageArrayLayers=1;
    si.imageUsage=VK_IMAGE_USAGE_TRANSFER_DST_BIT; si.imageSharingMode=VK_SHARING_MODE_EXCLUSIVE;
    si.preTransform=caps.currentTransform; si.compositeAlpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    require(caps.supportedCompositeAlpha&si.compositeAlpha,"opaque surface required");
    si.presentMode=VK_PRESENT_MODE_FIFO_KHR; si.clipped=VK_TRUE;
    VkSwapchainKHR swapchain{}; VK(vkCreateSwapchainKHR(e.device,&si,nullptr,&swapchain));
    VK(vkGetSwapchainImagesKHR(e.device,swapchain,&count,nullptr));
    std::vector<VkImage> images(count); VK(vkGetSwapchainImagesKHR(e.device,swapchain,&count,images.data()));
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore acquired{},finished{};
    VK(vkCreateSemaphore(e.device,&sci,nullptr,&acquired));
    VK(vkCreateSemaphore(e.device,&sci,nullptr,&finished));
    uint32_t index{}; VK(vkAcquireNextImageKHR(e.device,swapchain,30'000'000'000ull,acquired,nullptr,&index));
    VkCommandBuffer cmd=e.command(); begin(cmd);
    barrier(cmd,output,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_QUEUE_FAMILY_EXTERNAL,e.family);
    Image target{}; target.image=images[index]; target.aspect=VK_IMAGE_ASPECT_COLOR_BIT;
    barrier(cmd,target,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit blit{};
    blit.srcSubresource={output.aspect,0,0,1}; blit.srcOffsets[1]={(int)OW,(int)OH,1};
    blit.dstSubresource={target.aspect,0,0,1};
    blit.dstOffsets[1]={(int)extent.width,(int)extent.height,1};
    vkCmdBlitImage(cmd,output.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,target.image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&blit,VK_FILTER_NEAREST);
    barrier(cmd,target,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    barrier(cmd,output,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL,
            e.family,VK_QUEUE_FAMILY_EXTERNAL);
    VK(vkEndCommandBuffer(cmd));
    VkSemaphore waits[]={scaled,acquired}; uint64_t values[]={value,0},signals[]={0};
    VkPipelineStageFlags stages[]={VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
    VkTimelineSemaphoreSubmitInfo time{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    time.waitSemaphoreValueCount=2; time.pWaitSemaphoreValues=values;
    time.signalSemaphoreValueCount=1; time.pSignalSemaphoreValues=signals;
    VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submitInfo.pNext=&time;
    submitInfo.waitSemaphoreCount=2; submitInfo.pWaitSemaphores=waits; submitInfo.pWaitDstStageMask=stages;
    submitInfo.signalSemaphoreCount=1; submitInfo.pSignalSemaphores=&finished;
    submitInfo.commandBufferCount=1; submitInfo.pCommandBuffers=&cmd;
    VK(vkQueueSubmit(e.queue,1,&submitInfo,nullptr));
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount=1; pi.pWaitSemaphores=&finished;
    pi.swapchainCount=1; pi.pSwapchains=&swapchain; pi.pImageIndices=&index;
    VK(vkQueuePresentKHR(e.queue,&pi));
    VK(vkQueueWaitIdle(e.queue));
    SDL_PumpEvents();
    vkDestroySwapchainKHR(e.device,swapchain,nullptr);
    vkDestroySemaphore(e.device,acquired,nullptr); vkDestroySemaphore(e.device,finished,nullptr);
    vkDestroySurfaceKHR(e.instance,surface,nullptr); SDL_DestroyWindow(window); SDL_Quit();
    std::puts("PASS: temporal MetalFX output -> original VkImage -> Vulkan swapchain present (presentation blit only)");
}

static FfxVkPortableImage describe(const Image& x,FfxVkPortableResourceState state) {
    return {sizeof(FfxVkPortableImage),x.image,x.format,{x.w,x.h},1,1,x.usage,x.aspect,state};
}
int main(int argc,char** argv) {
    setbuf(stdout,nullptr);
    try {
        bool wantPresent=false, depthStencil=false, useModule=false, reactiveHistory=false;
        for(int i=1;i<argc;++i) {
            if(!std::strcmp(argv[i],"--present")) wantPresent=true;
            else if(!std::strcmp(argv[i],"--depth-stencil")) depthStencil=true;
            else if(!std::strcmp(argv[i],"--module")) useModule=true;
            else if(!std::strcmp(argv[i],"--reactive-history")) reactiveHistory=true;
            else throw std::runtime_error("expected --present, --depth-stencil, --module or --reactive-history");
        } @autoreleasepool {
        require(!reactiveHistory || useModule,"reactive-history qualification requires --module");
        int translated=0; size_t n=sizeof(translated);
        require(sysctlbyname("sysctl.proc_translated",&translated,&n,nullptr,0)==0 && translated==1,
                "prototype requires actual Rosetta x86_64");
        Env e;
        require([MTLFXTemporalScalerDescriptor supportsDevice:e.metal],"MetalFX temporal unsupported");
        require([MTLFXSpatialScalerDescriptor supportsDevice:e.metal],"MetalFX spatial unsupported");
        auto* td=[MTLFXTemporalScalerDescriptor new];
        td.inputWidth=W; td.inputHeight=H; td.outputWidth=OW; td.outputHeight=OH;
        td.colorTextureFormat=MTLPixelFormatRGBA16Float;
        td.depthTextureFormat=depthStencil?MTLPixelFormatDepth32Float_Stencil8:MTLPixelFormatDepth32Float;
        td.motionTextureFormat=MTLPixelFormatRG16Float; td.outputTextureFormat=MTLPixelFormatRGBA16Float;
        td.autoExposureEnabled=YES; td.requiresSynchronousInitialization=YES;
        id<MTLFXTemporalScaler> temporal=[td newTemporalScalerWithDevice:e.metal];
        require(temporal,"MetalFX temporal creation failed");
        auto* sd=[MTLFXSpatialScalerDescriptor new];
        sd.inputWidth=W; sd.inputHeight=H; sd.outputWidth=OW; sd.outputHeight=OH;
        sd.colorTextureFormat=MTLPixelFormatRGBA16Float; sd.outputTextureFormat=MTLPixelFormatRGBA16Float;
        sd.colorProcessingMode=MTLFXSpatialScalerColorProcessingModeHDR;
        id<MTLFXSpatialScaler> spatial=[sd newSpatialScalerWithDevice:e.metal];
        require(spatial,"MetalFX spatial creation failed");
        Image color=e.image(VK_FORMAT_R16G16B16A16_SFLOAT,W,H,VK_IMAGE_ASPECT_COLOR_BIT);
        Image depth=e.image(depthStencil?VK_FORMAT_D32_SFLOAT_S8_UINT:VK_FORMAT_D32_SFLOAT,
                            W,H,VK_IMAGE_ASPECT_DEPTH_BIT);
        require(depth.texture.pixelFormat==td.depthTextureFormat,"exported depth format mismatch");
        std::printf("Depth format Metal=%lu Vulkan=%d direct base texture\n",
                    (unsigned long)depth.texture.pixelFormat,depth.format);
        Image motion=e.image(VK_FORMAT_R16G16_SFLOAT,W,H,VK_IMAGE_ASPECT_COLOR_BIT);
        Image reactive=e.image(VK_FORMAT_R8_UNORM,W,H,VK_IMAGE_ASPECT_COLOR_BIT);
        Image output=e.image(VK_FORMAT_R16G16B16A16_SFLOAT,OW,OH,VK_IMAGE_ASPECT_COLOR_BIT);
        require(output.texture.storageMode==MTLStorageModePrivate,"MetalFX output must be private");
        auto usage=[](id<MTLTexture> t,MTLTextureUsage need) {require((t.usage&need)==need,"exported texture lacks MetalFX usage");};
        usage(color.texture,temporal.colorTextureUsage|spatial.colorTextureUsage);
        usage(depth.texture,temporal.depthTextureUsage); usage(motion.texture,temporal.motionTextureUsage);
        usage(output.texture,temporal.outputTextureUsage|spatial.outputTextureUsage);
        temporal.colorTexture=color.texture; temporal.depthTexture=depth.texture;
        temporal.motionTexture=motion.texture; temporal.outputTexture=output.texture;
        temporal.inputContentWidth=W; temporal.inputContentHeight=H;
        temporal.preExposure=1; temporal.motionVectorScaleX=1; temporal.motionVectorScaleY=1;
        temporal.depthReversed=NO;
        spatial.colorTexture=color.texture; spatial.outputTexture=output.texture;
        spatial.inputContentWidth=W; spatial.inputContentHeight=H;
        auto [ready,readyMetal]=event(e); auto [done,doneMetal]=event(e);
        std::unique_ptr<BbMetalFX::TemporalScaler> provider;
        std::shared_ptr<BbMetalFX::Textures> binding;
        if(useModule) {
            provider=std::make_unique<BbMetalFX::TemporalScaler>(e.device,vkGetDeviceProcAddr,
                ready,done,BbMetalFX::Config{W,H,OW,OH,depthStencil});
            binding=provider->Register({color.image,depth.image,motion.image,output.image,reactive.image});
        }
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fence{};
        VK(vkCreateFence(e.device,&fi,nullptr,&fence));
        VkCommandBuffer producer=e.command(), consumer=e.command();
        VkBufferCreateInfo bc{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bc.size=8;
        bc.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VkBuffer readback{}; VK(vkCreateBuffer(e.device,&bc,nullptr,&readback));
        VkMemoryRequirements br{}; vkGetBufferMemoryRequirements(e.device,readback,&br);
        VkMemoryAllocateInfo ba{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ba.allocationSize=br.size;
        ba.memoryTypeIndex=e.memoryType(br.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkDeviceMemory bm{}; VK(vkAllocateMemory(e.device,&ba,nullptr,&bm));
        VK(vkBindBufferMemory(e.device,readback,bm,0));
        void* pixel{}; VK(vkMapMemory(e.device,bm,0,VK_WHOLE_SIZE,0,&pixel));
        VkAttachmentDescription attachment{};
        attachment.format=color.format; attachment.samples=VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR; attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
        attachment.initialLayout=VK_IMAGE_LAYOUT_GENERAL; attachment.finalLayout=VK_IMAGE_LAYOUT_GENERAL;
        VkAttachmentReference ref{0,VK_IMAGE_LAYOUT_GENERAL};
        VkSubpassDescription sub{}; sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount=1; sub.pColorAttachments=&ref;
        VkRenderPassCreateInfo rc{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rc.attachmentCount=1; rc.pAttachments=&attachment; rc.subpassCount=1; rc.pSubpasses=&sub;
        VkRenderPass renderPass{}; VK(vkCreateRenderPass(e.device,&rc,nullptr,&renderPass));
        VkFramebufferCreateInfo fc{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fc.renderPass=renderPass; fc.attachmentCount=1; fc.pAttachments=&color.view;
        fc.width=W; fc.height=H; fc.layers=1;
        VkFramebuffer frame{}; VK(vkCreateFramebuffer(e.device,&fc,nullptr,&frame));
        for(uint64_t value=1; value<=16; ++value) {
            const float red=value%2 ? 0.25f : 0.75f;
            begin(producer);
            for(auto* x:{&color,&depth,&motion,&output,&reactive})
                barrier(producer,*x,value==1?VK_IMAGE_LAYOUT_UNDEFINED:VK_IMAGE_LAYOUT_GENERAL,
                        VK_IMAGE_LAYOUT_GENERAL,value==1?VK_QUEUE_FAMILY_IGNORED:VK_QUEUE_FAMILY_EXTERNAL,
                        value==1?VK_QUEUE_FAMILY_IGNORED:e.family);
            VkClearValue clear{}; clear.color.float32[0]=red;
            clear.color.float32[1]=0.5f; clear.color.float32[2]=0.25f; clear.color.float32[3]=1;
            VkRenderPassBeginInfo ri{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO}; ri.renderPass=renderPass;
            ri.framebuffer=frame; ri.renderArea={{0,0},{W,H}}; ri.clearValueCount=1; ri.pClearValues=&clear;
            vkCmdBeginRenderPass(producer,&ri,VK_SUBPASS_CONTENTS_INLINE); vkCmdEndRenderPass(producer);
            VkImageSubresourceRange dr{depth.aspect,0,1,0,1}, mr{motion.aspect,0,1,0,1};
            VkClearDepthStencilValue dv{0.5f,0}; VkClearColorValue mv{};
            vkCmdClearDepthStencilImage(producer,depth.image,VK_IMAGE_LAYOUT_GENERAL,&dv,1,&dr);
            vkCmdClearColorImage(producer,motion.image,VK_IMAGE_LAYOUT_GENERAL,&mv,1,&mr);
            VkClearColorValue reactiveValue{};
            reactiveValue.float32[0]=reactiveHistory?1.0f:0.0f;
            vkCmdClearColorImage(producer,reactive.image,VK_IMAGE_LAYOUT_GENERAL,&reactiveValue,1,&mr);
            for(auto* x:{&color,&depth,&motion,&output,&reactive})
                barrier(producer,*x,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL,e.family,VK_QUEUE_FAMILY_EXTERNAL);
            submit(e,producer,nullptr,0,ready,value,nullptr);
            id<MTLCommandBuffer> cb=nil;
            if(provider && value<=8) {
                provider->Submit(binding,{value,value,value-1,reactiveHistory?0:value,0,0});
            } else {
                cb=[e.metalQueue commandBuffer];
                [cb encodeWaitForEvent:readyMetal value:value];
                if(value<=8) {temporal.reset=YES; [temporal encodeToCommandBuffer:cb];}
                else [spatial encodeToCommandBuffer:cb];
                [cb encodeSignalEvent:doneMetal value:value]; [cb commit];
            }
            begin(consumer);
            barrier(consumer,output,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_QUEUE_FAMILY_EXTERNAL,e.family);
            VkBufferImageCopy copy{}; copy.imageSubresource={output.aspect,0,0,1};
            copy.imageOffset={(int)OW/2,(int)OH/2,0}; copy.imageExtent={1,1,1};
            vkCmdCopyImageToBuffer(consumer,output.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback,1,&copy);
            barrier(consumer,output,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL,
                    e.family,VK_QUEUE_FAMILY_EXTERNAL);
            submit(e,consumer,done,value,nullptr,0,fence); complete(e,fence);
            if(cb) metalComplete(cb); else provider->Drain();
            _Float16 actual[4]; std::memcpy(actual,pixel,8);
            std::printf("interop frame=%llu scaler=%s rgb=%.4f,%.4f,%.4f expected=%.2f,.5,.25\n",
               (unsigned long long)value,value<=8?"temporal":"spatial",
               (float)actual[0],(float)actual[1],(float)actual[2],red);
            require(std::isfinite((float)actual[0]) && std::abs((float)actual[0]-red)<0.08f &&
                    std::abs((float)actual[1]-0.5f)<0.08f && std::abs((float)actual[2]-0.25f)<0.08f,
                    "Vulkan -> MetalFX -> Vulkan pixel mismatch");
        }
        std::puts("PASS: same-texture interop, separate GPU queues, shared timeline events; 16 alternating frames");
        if(provider) std::puts("PASS: standalone signed Apple module, cached exports, history reset and GPU event interop");
        if(reactiveHistory) std::puts("PASS: reactive mask=1 rejects alternating color history with a continuous epoch");
        if(wantPresent) {
            id<MTLCommandBuffer> cb=nil;
            if(provider) provider->Submit(binding,{16,17,16,17,0,0});
            else {
                cb=[e.metalQueue commandBuffer];
                [cb encodeWaitForEvent:readyMetal value:16];
                temporal.reset=YES; [temporal encodeToCommandBuffer:cb];
                [cb encodeSignalEvent:doneMetal value:17]; [cb commit];
            }
            present(e,output,done,17);
            if(provider) provider->Drain(); else metalComplete(cb);
        }
        if(provider) {
            // The renderer may destroy the provider after its encoding thread has stopped, with a
            // frame still in flight: the destructor must not require the encoding thread (it
            // aborted here before) and must wait for that frame. The frame's Metal work waits for
            // a producer value the host signals only after the destroying thread has started.
            constexpr uint64_t producer=1000, consumer=1001;
            provider->Submit(binding,{producer,consumer,1000,999,0,0});
            const auto start=std::chrono::steady_clock::now();
            double destroy_ms=0;
            std::thread destroyer([&]{
                provider.reset();
                destroy_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            VkSemaphoreSignalInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
            signal.semaphore=ready; signal.value=producer;
            VK(vkSignalSemaphore(e.device,&signal));
            destroyer.join();
            uint64_t completed=0; VK(vkGetSemaphoreCounterValue(e.device,done,&completed));
            require(completed>=consumer,"provider destructor returned before its in-flight frame completed");
            require(destroy_ms>=90,"provider destructor did not wait for its in-flight frame");
            std::printf("PASS: provider destroyed off its encoding thread after waiting %.0f ms for its in-flight frame\n",destroy_ms);
        }
        for(bool isTemporal:{true,false}) {
            std::vector<double> times;
            for(int i=0;i<72;++i) { @autoreleasepool {
                auto cb=[e.metalQueue commandBuffer];
                if(isTemporal) { temporal.reset=(i==0); [temporal encodeToCommandBuffer:cb]; }
                else [spatial encodeToCommandBuffer:cb];
                [cb commit]; metalComplete(cb);
                double ms=(cb.GPUEndTime-cb.GPUStartTime)*1000;
                require(ms>0 && std::isfinite(ms),"Metal GPU timing unavailable");
                if(i>=12) times.push_back(ms);
            }}
            report(isTemporal?"MetalFX temporal1440p->2160p":"MetalFX spatial1440p->2160p",times);
        }
        FfxVkPortableDeviceInfo device{};
        device.structSize=sizeof(device); device.instance=e.instance; device.physicalDevice=e.physical;
        device.device=e.device; device.getDeviceProcAddr=vkGetDeviceProcAddr;
        device.queue=e.queue; device.queueFamilyIndex=e.family;
        device.shaderFloat16Enabled=e.fp16; device.synchronization2Enabled=VK_TRUE;
        device.shaderStorageImageWriteWithoutFormatEnabled=VK_TRUE;
        FfxVkPortableUpscaleCreateInfo create{};
        create.structSize=sizeof(create); create.flags=FFX_VK_PORTABLE_CONTEXT_HDR_COLOR_INPUT|FFX_VK_PORTABLE_CONTEXT_AUTO_EXPOSURE;
        create.maxRenderSize={W,H}; create.maxOutputSize={OW,OH};
        FfxVkPortableUpscaleContext* context{};
        require(ffxVkPortableUpscaleContextCreate(&device,&create,&context)==FFX_VK_PORTABLE_OK,
                "isolated FSR3 context creation failed");
        VkQueryPoolCreateInfo qc{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qc.queryType=VK_QUERY_TYPE_TIMESTAMP; qc.queryCount=2;
        VkQueryPool queries{}; VK(vkCreateQueryPool(e.device,&qc,nullptr,&queries));
        for(bool sharpen:{false,true}) {
        std::vector<double> fsrTimes;
        for(int i=0;i<72;++i) {
            begin(producer);
            if(i==0 && !sharpen) for(auto* x:{&color,&depth,&motion,&output})
                barrier(producer,*x,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL,VK_QUEUE_FAMILY_EXTERNAL,e.family);
            FfxVkPortableUpscaleDispatchInfo dispatch{};
            dispatch.structSize=sizeof(dispatch); dispatch.commandBuffer=producer;
            dispatch.color=describe(color,FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
            dispatch.depth=describe(depth,FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
            dispatch.motionVectors=describe(motion,FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
            dispatch.output=describe(output,FFX_VK_PORTABLE_RESOURCE_STATE_UNORDERED_ACCESS);
            dispatch.exposure.structSize=sizeof(dispatch.exposure);
            dispatch.reactiveMask.structSize=sizeof(dispatch.reactiveMask);
            dispatch.transparencyAndCompositionMask.structSize=sizeof(dispatch.transparencyAndCompositionMask);
            dispatch.motionVectorScale={1,1}; dispatch.renderSize={W,H}; dispatch.outputSize={OW,OH};
            dispatch.frameTimeMilliseconds=16.667f; dispatch.preExposure=1; dispatch.cameraNear=0.05f;
            dispatch.cameraFar=3000; dispatch.cameraVerticalFovRadians=0.75f;
            dispatch.viewSpaceToMeters=1; dispatch.reset=(i==0); dispatch.frameId=i+(sharpen?72:0);
            dispatch.enableSharpening=sharpen; dispatch.sharpness=0.3f;
            vkCmdResetQueryPool(producer,queries,0,2);
            vkCmdWriteTimestamp(producer,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,queries,0);
            require(ffxVkPortableUpscaleContextRecordDispatch(context,&dispatch)==FFX_VK_PORTABLE_OK,
                    "isolated FSR3 dispatch failed");
            vkCmdWriteTimestamp(producer,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,queries,1);
            submit(e,producer,nullptr,0,nullptr,0,fence); complete(e,fence);
            uint64_t stamps[2]{};
            VK(vkGetQueryPoolResults(e.device,queries,0,2,sizeof(stamps),stamps,sizeof(uint64_t),
                                    VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WAIT_BIT));
            double ms=(stamps[1]-stamps[0])*e.properties.limits.timestampPeriod/1e6;
            require(ms>0 && std::isfinite(ms),"Vulkan timestamp unavailable");
            if(i>=12) fsrTimes.push_back(ms);
        }
        report(sharpen?"FSR3.1 temporal1440p->2160p RCAS0.3":"FSR3.1 temporal1440p->2160p noRCAS",fsrTimes);
        begin(consumer);
        barrier(consumer,output,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy copy{}; copy.imageSubresource={output.aspect,0,0,1};
        copy.imageOffset={(int)OW/2,(int)OH/2,0}; copy.imageExtent={1,1,1};
        vkCmdCopyImageToBuffer(consumer,output.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback,1,&copy);
        barrier(consumer,output,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL);
        submit(e,consumer,nullptr,0,nullptr,0,fence); complete(e,fence);
        _Float16 actual[4]; std::memcpy(actual,pixel,8);
        std::printf("FSR3 RGB=%.4f,%.4f,%.4f RCAS=%d\n",
                    (float)actual[0],(float)actual[1],(float)actual[2],sharpen);
        require(std::isfinite((float)actual[0]) && std::abs((float)actual[0]-0.75f)<0.08f &&
                std::abs((float)actual[1]-0.5f)<0.08f && std::abs((float)actual[2]-0.25f)<0.08f,
                "isolated FSR3 output pixel mismatch");
        }
        require(ffxVkPortableUpscaleContextDestroy(context)==FFX_VK_PORTABLE_OK,"FSR3 destroy failed");
        VK(vkDeviceWaitIdle(e.device));
        std::puts("PASS: MetalFX Rosetta encodes + isolated FSR3 Vulkan benchmark; no renderer changes");
        return 0;
    }} catch(const std::exception& error) {
        std::fprintf(stderr,"FAIL MetalFX prototype: %s\n",error.what()); return 1;
    }
}
