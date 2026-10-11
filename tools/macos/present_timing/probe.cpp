#include <vulkan/vulkan.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

static void check(bool ok, const char* text) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", text); std::exit(1); }
}
#define VK(call) check((call) == VK_SUCCESS, #call)
using Clock = std::chrono::steady_clock;
static uint64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}
int main() {
    setbuf(stdout, nullptr);
    check(SDL_Init(SDL_INIT_VIDEO), SDL_GetError());
    auto* window = SDL_CreateWindow("Display completion qualification", 320, 180, SDL_WINDOW_VULKAN);
    check(window, SDL_GetError());
    uint32_t ni{};
    const char* const* sdl_ext = SDL_Vulkan_GetInstanceExtensions(&ni);
    std::vector<const char*> ie(sdl_ext, sdl_ext + ni);
    ie.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    ci.pApplicationInfo = &app; ci.enabledExtensionCount = ie.size(); ci.ppEnabledExtensionNames = ie.data();
    VkInstance instance{}; VK(vkCreateInstance(&ci, nullptr, &instance));
    uint32_t count{}; VK(vkEnumeratePhysicalDevices(instance, &count, nullptr));
    check(count == 1, "one ICD device required");
    VkPhysicalDevice physical{}; VK(vkEnumeratePhysicalDevices(instance, &count, &physical));
    VkSurfaceKHR surface{};
    check(SDL_Vulkan_CreateSurface(window, instance, nullptr, &surface), SDL_GetError());
    VK(vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr));
    std::vector<VkExtensionProperties> extensions(count);
    VK(vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, extensions.data()));
    const char* de[]={"VK_KHR_portability_subset", VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME, VK_KHR_PRESENT_ID_EXTENSION_NAME,
        VK_KHR_PRESENT_WAIT_EXTENSION_NAME};
    for (auto name : de) check(std::any_of(extensions.begin(), extensions.end(), [&](auto& e) {
        return !std::strcmp(e.extensionName, name); }), name);
    VkPhysicalDevicePresentWaitFeaturesKHR pw{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR};
    VkPhysicalDevicePresentIdFeaturesKHR pid{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.pNext=&pid; pid.pNext=&pw;
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2}; features.pNext=&f12;
    vkGetPhysicalDeviceFeatures2(physical, &features);
    check(f12.timelineSemaphore && pid.presentId && pw.presentWait, "required features");
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
    uint32_t family=0;
    while (family<count && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) ++family;
    check(family<count, "graphics queue");
    VkBool32 supported{}; VK(vkGetPhysicalDeviceSurfaceSupportKHR(physical, family, surface, &supported));
    check(supported, "surface support");
    float priority=1;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex=family; qi.queueCount=1; qi.pQueuePriorities=&priority;
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; di.pNext=&features;
    di.queueCreateInfoCount=1; di.pQueueCreateInfos=&qi; di.enabledExtensionCount=5; di.ppEnabledExtensionNames=de;
    VkDevice device{}; VK(vkCreateDevice(physical, &di, nullptr, &device));
    VkQueue queue{}; vkGetDeviceQueue(device, family, 0, &queue);
    auto timing=reinterpret_cast<PFN_vkGetPastPresentationTimingGOOGLE>(vkGetDeviceProcAddr(device,"vkGetPastPresentationTimingGOOGLE"));
    auto wait_present=reinterpret_cast<PFN_vkWaitForPresentKHR>(vkGetDeviceProcAddr(device,"vkWaitForPresentKHR"));
    check(timing && wait_present, "completion entrypoints");
    VkSurfaceCapabilitiesKHR caps{}; VK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical,surface,&caps));
    VK(vkGetPhysicalDeviceSurfaceFormatsKHR(physical,surface,&count,nullptr));
    std::vector<VkSurfaceFormatKHR> formats(count); VK(vkGetPhysicalDeviceSurfaceFormatsKHR(physical,surface,&count,formats.data()));
    VkSwapchainCreateInfoKHR si{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    si.surface=surface; si.minImageCount=std::max(caps.minImageCount,3u);
    if(caps.maxImageCount) si.minImageCount=std::min(si.minImageCount,caps.maxImageCount);
    si.imageFormat=formats[0].format; si.imageColorSpace=formats[0].colorSpace;
    si.imageExtent=caps.currentExtent; si.imageArrayLayers=1; si.imageUsage=VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    si.imageSharingMode=VK_SHARING_MODE_EXCLUSIVE; si.preTransform=caps.currentTransform;
    si.compositeAlpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR; si.presentMode=VK_PRESENT_MODE_FIFO_KHR; si.clipped=VK_TRUE;
    VkSwapchainKHR swap{}; VK(vkCreateSwapchainKHR(device,&si,nullptr,&swap));
    VK(vkGetSwapchainImagesKHR(device,swap,&count,nullptr));
    std::vector<VkImage> images(count); VK(vkGetSwapchainImagesKHR(device,swap,&count,images.data()));
    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pool_info.queueFamilyIndex=family;
    VkCommandPool pool{}; VK(vkCreateCommandPool(device,&pool_info,nullptr,&pool));
    constexpr uint32_t frames=30;
    std::vector<VkSemaphore> acquired(frames),ready(count);
    VkSemaphoreCreateInfo sem_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for(auto& s:acquired) VK(vkCreateSemaphore(device,&sem_info,nullptr,&s));
    for(auto& s:ready) VK(vkCreateSemaphore(device,&sem_info,nullptr,&s));
    VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO}; type.semaphoreType=VK_SEMAPHORE_TYPE_TIMELINE;
    sem_info.pNext=&type; VkSemaphore done{}, gate{};
    VK(vkCreateSemaphore(device,&sem_info,nullptr,&done));
    VK(vkCreateSemaphore(device,&sem_info,nullptr,&gate));
    std::vector<Clock::time_point> submitted(frames+1);
    std::vector<bool> seen(frames+1);
    uint32_t observed=0;
    std::atomic<uint64_t> gate_release_ns{};
    uint32_t held_gate_observations=0;
    auto poll=[&] {
        SDL_PumpEvents();
        VkPastPresentationTimingGOOGLE rows[64]{}; uint32_t n=64;
        auto result=timing(device,swap,&n,rows);
        check(result==VK_SUCCESS || result==VK_INCOMPLETE,"timing query");
        uint64_t completed{}; VK(vkGetSemaphoreCounterValue(device,done,&completed));
        if(!gate_release_ns.load(std::memory_order_acquire)) {
            check(completed==0 && n==0,"completion observed while host gate remained unsignaled");
            ++held_gate_observations;
        }
        for(uint32_t i=0;i<n;++i) {
            auto id=rows[i].presentID;
            check(id && id<=frames && !seen[id],"unique known present id");
            check(completed>=id,"rendering incomplete at display-report observation");
            check(rows[i].actualPresentTime>0,"nonzero actual presentation time");
            seen[id]=true; ++observed;
            auto us=std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-submitted[id]).count();
            std::printf("display id=%u gpu_tick=%llu actual_ns=%llu observed_after_us=%lld\n",id,
                (unsigned long long)completed,(unsigned long long)rows[i].actualPresentTime,(long long)us);
        }
    };
    for(uint32_t f=0;f<frames;++f) {
        uint32_t index{}; VK(vkAcquireNextImageKHR(device,swap,5'000'000'000ull,acquired[f],nullptr,&index));
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool=pool; ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount=1;
        VkCommandBuffer cmd{}; VK(vkAllocateCommandBuffers(device,&ai,&cmd));
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; VK(vkBeginCommandBuffer(cmd,&bi));
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED; barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        barrier.image=images[index]; barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
        VkClearColorValue color{}; color.float32[0]=(f%2)*0.1f; color.float32[3]=1;
        vkCmdClearColorImage(cmd,images[index],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&color,1,&barrier.subresourceRange);
        barrier.oldLayout=barrier.newLayout; barrier.newLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask=0;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,nullptr,0,nullptr,1,&barrier);
        VK(vkEndCommandBuffer(cmd));
        VkSemaphore signals[]={ready[index],done}, waits[]={acquired[f],gate};
        uint64_t values[]={0,f+1u}, wait_values[]={0,1};
        VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
        timeline.waitSemaphoreValueCount=2; timeline.pWaitSemaphoreValues=wait_values;
        timeline.signalSemaphoreValueCount=2; timeline.pSignalSemaphoreValues=values;
        VkPipelineStageFlags stages[]={VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.pNext=&timeline;
        submit.waitSemaphoreCount=2; submit.pWaitSemaphores=waits; submit.pWaitDstStageMask=stages;
        submit.commandBufferCount=1; submit.pCommandBuffers=&cmd; submit.signalSemaphoreCount=2; submit.pSignalSemaphores=signals;
        std::thread release_gate;
        if(f==0) release_gate=std::thread([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            VkSemaphoreSignalInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
            signal.semaphore=gate; signal.value=1;
            gate_release_ns.store(now_ns(),std::memory_order_release);
            VK(vkSignalSemaphore(device,&signal));
        });
        const auto submit_begin=now_ns();
        VK(vkQueueSubmit(queue,1,&submit,nullptr));
        const auto submit_end=now_ns();
        uint64_t id=f+1; VkPresentIdKHR ids{VK_STRUCTURE_TYPE_PRESENT_ID_KHR}; ids.swapchainCount=1; ids.pPresentIds=&id;
        VkPresentTimeGOOGLE time{f+1,0}; VkPresentTimesInfoGOOGLE times{VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE};
        times.pNext=&ids; times.swapchainCount=1; times.pTimes=&time;
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR}; pi.pNext=&times; pi.waitSemaphoreCount=1;
        pi.pWaitSemaphores=&ready[index]; pi.swapchainCount=1; pi.pSwapchains=&swap; pi.pImageIndices=&index;
        submitted[id]=Clock::now(); const auto present_begin=now_ns();
        VK(vkQueuePresentKHR(queue,&pi));
        const auto present_end=now_ns();
        std::printf("calls id=%llu submit_begin_ns=%llu submit_end_ns=%llu present_begin_ns=%llu present_end_ns=%llu\n",
            (unsigned long long)id,(unsigned long long)submit_begin,(unsigned long long)submit_end,
            (unsigned long long)present_begin,(unsigned long long)present_end);
        for(int j=0;j<20;++j) { poll(); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
        auto r=wait_present(device,swap,id,0);
        check(r==VK_SUCCESS || r==VK_TIMEOUT,"present wait query");
        const auto elapsed=std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-submitted[id]).count();
        std::printf("after_poll id=%llu elapsed_us=%lld wait_done=%d display_seen=%d\n",
            (unsigned long long)id,(long long)elapsed,r==VK_SUCCESS,seen[id]?1:0);
        if(release_gate.joinable()) release_gate.join();
    }
    auto deadline=Clock::now()+std::chrono::seconds(2);
    while(observed<frames && Clock::now()<deadline) { poll(); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    std::printf("RESULT presented=%u/%u; final callbacks missing=%u; gate_release_ns=%llu held_gate_observations=%u (%s)\n",
        observed,frames,frames-observed,(unsigned long long)gate_release_ns.load(),held_gate_observations,
        held_gate_observations ? "observed" : "UNOBSERVED negative control");
    VK(vkDeviceWaitIdle(device));
    vkDestroySwapchainKHR(device,swap,nullptr);
    vkDestroyCommandPool(device,pool,nullptr);
    for(auto s:acquired) vkDestroySemaphore(device,s,nullptr);
    for(auto s:ready) vkDestroySemaphore(device,s,nullptr);
    vkDestroySemaphore(device,done,nullptr); vkDestroySemaphore(device,gate,nullptr);
    vkDestroyDevice(device,nullptr);
    vkDestroySurfaceKHR(instance,surface,nullptr); vkDestroyInstance(instance,nullptr);
    SDL_DestroyWindow(window); SDL_Quit();
    return observed!=frames ? 2 : (held_gate_observations ? 0 : 3);
}
