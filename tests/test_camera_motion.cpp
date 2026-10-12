// SPDX-License-Identifier: GPL-2.0-or-later
// Execute production camera motion SPIR-V: distant geometry, sky, stale object vectors, and
// vertical motion (pitch, vertical translation) against a reference that inverts the scene
// viewport (pixel = offset + ndc * scale), for both y signs, an offset and scaled viewport, and a
// previous viewport that differs from the current one.
#include <array>
#include <cassert>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <tuple>
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/image.h"
#include "video_core/host_shaders/camera_motion_comp.h"
#include <vk_mem_alloc.h>

int main() {
    Vulkan::Instance instance(0, false);
    static vk::detail::DynamicLoader loader;
    vk::detail::DispatchLoaderDynamic d;
    d.init(loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr"));
    d.init(instance.GetInstance()); d.init(instance.GetDevice());
    const auto device = instance.GetDevice();
    Vulkan::Scheduler scheduler(instance);
    constexpr u32 W=9,H=7,N=W*H;
    std::array<VideoCore::UniqueImage,3> images;
    std::array<vk::UniqueImageView,3> views;
    std::array<vk::DescriptorSetLayoutBinding,3> bindings{};
    for (u32 i=0;i<3;++i) {
        const auto format=i==1 ? vk::Format::eR16G16Sfloat : vk::Format::eR32G32B32A32Sfloat;
        images[i]=VideoCore::UniqueImage(device,instance.GetAllocator());
        images[i].Create({.imageType=vk::ImageType::e2D,.format=format,.extent={W,H,1},
            .mipLevels=1,.arrayLayers=1,.samples=vk::SampleCountFlagBits::e1,
            .tiling=vk::ImageTiling::eOptimal,.usage=vk::ImageUsageFlagBits::eSampled |
            vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferSrc |
            vk::ImageUsageFlagBits::eTransferDst});
        views[i]=device.createImageViewUnique({.image=vk::Image(images[i]),
            .viewType=vk::ImageViewType::e2D,.format=format,
            .subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,1}},nullptr,d).value;
        bindings[i]={.binding=i,.descriptorType=i==1 ? vk::DescriptorType::eStorageImage :
            vk::DescriptorType::eSampledImage,.descriptorCount=1,
            .stageFlags=vk::ShaderStageFlagBits::eCompute};
    }
    auto descriptors=device.createDescriptorSetLayoutUnique({
        .flags=vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount=3,.pBindings=bindings.data()},nullptr,d).value;
    struct Params {
        std::array<float,12> reproject;
        std::array<float,4> proj,prev_proj,viewport,prev_viewport;
        std::array<float,2> size,jitter,previous_jitter;
        u32 mode;
    };
    static_assert(sizeof(Params)==140); // over 128: the device must offer it (CameraMotion)
    assert(instance.MaxPushConstantsSize()>=sizeof(Params));
    const vk::PushConstantRange push{vk::ShaderStageFlagBits::eCompute,0,sizeof(Params)};
    auto layout=device.createPipelineLayoutUnique({.setLayoutCount=1,.pSetLayouts=&*descriptors,
        .pushConstantRangeCount=1,.pPushConstantRanges=&push},nullptr,d).value;
    auto module=device.createShaderModuleUnique({.codeSize=sizeof(CAMERA_MOTION_COMP),
        .pCode=CAMERA_MOTION_COMP},nullptr,d).value;
    auto pipeline=device.createComputePipelineUnique({}, {
        .stage={.stage=vk::ShaderStageFlagBits::eCompute,.module=*module,.pName="main"},
        .layout=*layout},nullptr,d).value;
    VkBuffer staging{}; VmaAllocation allocation{}; VmaAllocationInfo ai{};
    const VkBufferCreateInfo bi{.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size=N*16*2,.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    const VmaAllocationCreateInfo ac{.flags=VMA_ALLOCATION_CREATE_MAPPED_BIT |
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,.usage=VMA_MEMORY_USAGE_AUTO,
        .requiredFlags=VK_MEMORY_PROPERTY_HOST_COHERENT_BIT};
    assert(vmaCreateBuffer(instance.GetAllocator(),&bi,&ac,&staging,&allocation,&ai)==VK_SUCCESS);
    const auto barrier=[&](vk::CommandBuffer cmd,vk::PipelineStageFlags2 src,
                          vk::AccessFlags2 src_access,vk::PipelineStageFlags2 dst,
                          vk::AccessFlags2 dst_access) {
        const vk::MemoryBarrier2 b{.srcStageMask=src,.srcAccessMask=src_access,
                                  .dstStageMask=dst,.dstAccessMask=dst_access};
        cmd.pipelineBarrier2({.memoryBarrierCount=1,.pMemoryBarriers=&b},d);
    };
    using Viewport=std::array<float,4>; // x scale, y scale, x offset, y offset (pixels)
    // The full frame with view +y down the rows, and with +y up (a negative y scale).
    const Viewport down{W/2.f,H/2.f,W/2.f,H/2.f}, up{W/2.f,-(H/2.f),W/2.f,H/2.f};
    assert(up[1]==-3.5f); // W and H are unsigned: -H/2.f would be about +2.1e9
    // Reference, from the viewport contract only: pixel = offset + ndc * scale (Vulkan's viewport
    // transform, as the rasterizer sets it from the game's registers); ndc = proj.xy * view.xy /
    // view.z; view z from depth = proj.z + proj.w / z. Pixel centres, jitter removed.
    const auto reference_motion=[&](u32 px,u32 py,float depth,const std::array<float,12>& r,
                                    const Viewport& vp,const Viewport& prev_vp) {
        const float proj_x=1,proj_y=1,proj_z=1,proj_w=-0.05f;
        const float pos_x=px+0.5f,pos_y=py+0.5f;
        const float z=proj_w/(depth-proj_z);
        const float x=(pos_x-vp[2])/vp[0]*z/proj_x,y=(pos_y-vp[3])/vp[1]*z/proj_y;
        const float prev[3]={r[0]*x+r[1]*y+r[2]*z+r[3],r[4]*x+r[5]*y+r[6]*z+r[7],
                             r[8]*x+r[9]*y+r[10]*z+r[11]};
        const float prev_x=prev_vp[2]+prev[0]*proj_x/prev[2]*prev_vp[0];
        const float prev_y=prev_vp[3]+prev[1]*proj_y/prev[2]*prev_vp[1];
        return std::array<float,2>{prev_x-pos_x,prev_y-pos_y};
    };
    const auto run=[&](const char* label,float depth,float translation,float rotation,
                       float object_depth,u32 mode,float expected,bool jitter=false,
                       float rotation_y=0,float translation_y=0,float expected_y=0,
                       const std::array<float,12>* reproject=nullptr,u32 py=3,
                       const Viewport& viewport=Viewport{W/2.f,H/2.f,W/2.f,H/2.f},
                       const Viewport* prev_viewport=nullptr) {
        auto* pixels=static_cast<float*>(ai.pMappedData);
        for (u32 p=0;p<N;++p) {
            pixels[p*4]=depth;
            pixels[N*4+p*4]=2.f; pixels[N*4+p*4+1]=0.f;
            pixels[N*4+p*4+2]=1.f; pixels[N*4+p*4+3]=object_depth;
        }
        const auto cmd=scheduler.CommandBuffer();
        std::array<vk::ImageMemoryBarrier2,3> transitions{};
        for (u32 i=0;i<3;++i) transitions[i]={
            .srcStageMask=vk::PipelineStageFlagBits2::eAllCommands,
            .dstStageMask=vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask=vk::AccessFlagBits2::eMemoryRead|vk::AccessFlagBits2::eMemoryWrite,
            .oldLayout=vk::ImageLayout::eUndefined,.newLayout=vk::ImageLayout::eGeneral,
            .image=vk::Image(images[i]),.subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,1}};
        cmd.pipelineBarrier2({.imageMemoryBarrierCount=3,.pImageMemoryBarriers=transitions.data()},d);
        for (u32 i : {0u,2u}) {
            const vk::BufferImageCopy region{.bufferOffset=i/2*N*16,
                .imageSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},.imageExtent={W,H,1}};
            cmd.copyBufferToImage(staging,vk::Image(images[i]),vk::ImageLayout::eGeneral,region,d);
        }
        barrier(cmd,vk::PipelineStageFlagBits2::eTransfer,vk::AccessFlagBits2::eTransferWrite,
            vk::PipelineStageFlagBits2::eComputeShader,vk::AccessFlagBits2::eShaderRead);
        std::array<vk::DescriptorImageInfo,3> infos{};
        std::array<vk::WriteDescriptorSet,3> writes{};
        for (u32 i=0;i<3;++i) {
            infos[i]={.imageView=*views[i],.imageLayout=vk::ImageLayout::eGeneral};
            writes[i]={.dstBinding=i,.descriptorCount=1,.descriptorType=bindings[i].descriptorType,
                .pImageInfo=&infos[i]};
        }
        Params params{reproject ? *reproject :
            std::array<float,12>{1,0,rotation,translation,0,1,rotation_y,translation_y,0,0,1,0},
            {1,1,1,-0.05f},{1,1,1,-0.05f},viewport,prev_viewport ? *prev_viewport : viewport,{W,H},
            {jitter ? 0.37f : 0.f,jitter ? -0.29f : 0.f},{0,0},mode};
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute,*pipeline,d);
        cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute,*layout,0,writes,d);
        cmd.pushConstants(*layout,vk::ShaderStageFlagBits::eCompute,0,sizeof(params),&params,d);
        cmd.dispatch((W+7)/8,(H+7)/8,1,d);
        barrier(cmd,vk::PipelineStageFlagBits2::eComputeShader,vk::AccessFlagBits2::eShaderWrite,
            vk::PipelineStageFlagBits2::eTransfer,vk::AccessFlagBits2::eTransferRead);
        const vk::BufferImageCopy region{.imageSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},
            .imageOffset={4,s32(py),0},.imageExtent={1,1,1}};
        cmd.copyImageToBuffer(vk::Image(images[1]),vk::ImageLayout::eGeneral,staging,region,d);
        barrier(cmd,vk::PipelineStageFlagBits2::eTransfer,vk::AccessFlagBits2::eTransferWrite,
            vk::PipelineStageFlagBits2::eHost,vk::AccessFlagBits2::eHostRead);
        scheduler.Finish();
        const auto* result=static_cast<u16*>(ai.pMappedData);
        const auto half=[](u16 v) {
            const int e=(v>>10)&31;
            return (v&0x8000 ? -1.f : 1.f)*(e==0 ? std::ldexp(float(v&1023),-24) :
                std::ldexp(1.f+float(v&1023)/1024.f,e-15));
        };
        // Before the asserts, so a failing case shows what it measured (stdout and stderr).
        std::printf("Camera motion: %s measured (%.5f, %.5f), expected (%.5f, %.5f)\n",label,
                    half(result[0]),half(result[1]),expected,expected_y);
        std::fflush(stdout);
        assert(std::abs(half(result[0])-expected)<std::max(0.002f,std::abs(expected)*0.002f));
        assert(std::abs(half(result[1])-expected_y)<std::max(0.002f,std::abs(expected_y)*0.002f));
        std::printf("Camera motion: %s PASS (%.5f, %.5f)\n",label,half(result[0]),
                    half(result[1]));
    };
    constexpr float distant=0.999995f;
    run("distant geometry moves",distant,1000,0,0,0,4.5f*1000*(1-distant)/0.05f);
    run("sky rotates",1,0,0.1f,0,0,0.45f);
    run("sky ignores translation",1,1000,0,0,0,0);
    run("static camera cancels jitter",distant,0,0,0,0,0,true);
    run("matching object vector",distant,0,0,distant,1,2);
    run("stale distant object vector",distant,0,0,0.9999f,1,0);
    // Vertical motion, for each y sign of the viewport. Each case asserts that the other sign
    // (or, for the offset viewport, the full-frame mapping) predicts a clearly different motion,
    // so a shader that ignores the viewport's sign, offset or scale fails it. View y' = y +
    // pitch * z, or y + translation.
    const float near=0.99f; // view z 5 (not sky: the reference uses a finite z)
    const auto vertical=[&](const char* label,const std::array<float,12>& r,u32 py,
                            const Viewport& vp,const Viewport& other,
                            const Viewport* prev_vp=nullptr) {
        const auto expected=reference_motion(4,py,near,r,vp,prev_vp ? *prev_vp : vp);
        const auto wrong=reference_motion(4,py,near,r,other,prev_vp ? *prev_vp : other);
        std::printf("Camera motion: %s expects (%.5f, %.5f); the other mapping (%.5f, %.5f)\n",
                    label,expected[0],expected[1],wrong[0],wrong[1]);
        assert(std::abs(expected[0]-wrong[0])+std::abs(expected[1]-wrong[1])>0.1f);
        run(label,near,0,0,0,0,expected[0],false,0,0,expected[1],&r,py,vp,prev_vp);
    };
    const std::array<float,12> pitch{1,0,0,0,0,1,0.2f,0,0,0,1,0};
    const std::array<float,12> lift{1,0,0,0,0,1,0,0.5f,0,0,1,0};
    // Off-centre (row 1: view y != 0), view y feeding the previous x and z: the reconstruction's
    // y sign matters on its own here, not only together with the projection's.
    const std::array<float,12> y_to_xz{1,0.3f,0,0,0,1,0,0,0,0.5f,1,0};
    for (const auto& [name,vp,other] : {std::tuple{"y down",down,up},
                                        std::tuple{"y up",up,down}}) {
        const std::string n{name};
        vertical((n+": camera pitch moves rows").c_str(),pitch,3,vp,other);
        vertical((n+": vertical translation moves rows").c_str(),lift,3,vp,other);
        vertical((n+", off-centre: view y moves x and depth").c_str(),y_to_xz,1,vp,other);
        vertical((n+", off-centre: camera pitch").c_str(),pitch,1,vp,other);
    }
    // Neither centred nor full-frame, y up (a sub-rectangle of the target, rows 1 to 6, scaled).
    const Viewport sub{3.f,-2.5f,5.f,3.5f};
    vertical("offset, scaled viewport: camera pitch",pitch,1,sub,up);
    vertical("offset, scaled viewport: view y moves x and depth",y_to_xz,1,sub,up);
    // The previous frame's viewport projects the previous position (moved by a pixel here, with
    // the camera still): a shader projecting with the current viewport gives zero.
    {
        const std::array<float,12> still{1,0,0,0,0,1,0,0,0,0,1,0};
        const Viewport moved{sub[0],sub[1],sub[2]+1.f,sub[3]-1.f};
        const auto expected=reference_motion(4,1,near,still,sub,moved);
        assert(std::abs(expected[0]-1.f)<1e-5f && std::abs(expected[1]+1.f)<1e-5f);
        run("previous viewport projects the previous position",near,0,0,0,0,expected[0],false,0,0,
            expected[1],&still,1,sub,&moved);
    }
    // Jitter cancels with an offset viewport too (static camera).
    run("static camera cancels jitter, offset viewport",distant,0,0,0,0,0,true,0,0,0,nullptr,1,sub);
    vmaDestroyBuffer(instance.GetAllocator(),staging,allocation);
}
