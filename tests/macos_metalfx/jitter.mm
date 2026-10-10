// SPDX-License-Identifier: GPL-2.0-or-later
#define main original_interop_main
#include "interop.mm"
#undef main
#include <array>
#include <fstream>

int main(int argc, char** argv) {
    setbuf(stdout,nullptr);
    try { @autoreleasepool {
        require(argc==2,"provide the compiled jitter compute shader");
        std::ifstream file(argv[1],std::ios::binary|std::ios::ate);
        require(file.good(),"jitter SPIR-V missing");
        const auto size=file.tellg();
        require(size>0 && size%4==0,"invalid jitter SPIR-V size");
        std::vector<uint32_t> code(size_t(size)/4);
        file.seekg(0); file.read(reinterpret_cast<char*>(code.data()),size);
        require(file.good(),"failed to read jitter SPIR-V");
        Env e;
        Image color=e.image(VK_FORMAT_R16G16B16A16_SFLOAT,W,H,VK_IMAGE_ASPECT_COLOR_BIT);
        Image depth=e.image(VK_FORMAT_D32_SFLOAT,W,H,VK_IMAGE_ASPECT_DEPTH_BIT);
        Image motion=e.image(VK_FORMAT_R16G16_SFLOAT,W,H,VK_IMAGE_ASPECT_COLOR_BIT);
        Image reactive=e.image(VK_FORMAT_R8_UNORM,W,H,VK_IMAGE_ASPECT_COLOR_BIT);
        Image output=e.image(VK_FORMAT_R16G16B16A16_SFLOAT,OW,OH,VK_IMAGE_ASPECT_COLOR_BIT);
        auto [ready,readyMetal]=event(e); auto [done,doneMetal]=event(e);
        (void)readyMetal; (void)doneMetal;
        VkDescriptorSetLayoutBinding db{0,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
        VkDescriptorSetLayoutCreateInfo dc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        dc.bindingCount=1; dc.pBindings=&db;
        VkDescriptorSetLayout descriptors{}; VK(vkCreateDescriptorSetLayout(e.device,&dc,nullptr,&descriptors));
        VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT,0,4*sizeof(float)};
        VkPipelineLayoutCreateInfo lc{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        lc.setLayoutCount=1; lc.pSetLayouts=&descriptors; lc.pushConstantRangeCount=1; lc.pPushConstantRanges=&push;
        VkPipelineLayout layout{}; VK(vkCreatePipelineLayout(e.device,&lc,nullptr,&layout));
        VkShaderModuleCreateInfo sc{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; sc.codeSize=size_t(size); sc.pCode=code.data();
        VkShaderModule shader{}; VK(vkCreateShaderModule(e.device,&sc,nullptr,&shader));
        VkComputePipelineCreateInfo pc{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pc.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_COMPUTE_BIT,shader,"main",nullptr};
        pc.layout=layout;
        VkPipeline pipeline{}; VK(vkCreateComputePipelines(e.device,nullptr,1,&pc,nullptr,&pipeline));
        VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1};
        VkDescriptorPoolCreateInfo pd{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; pd.maxSets=1; pd.poolSizeCount=1; pd.pPoolSizes=&poolSize;
        VkDescriptorPool pool{}; VK(vkCreateDescriptorPool(e.device,&pd,nullptr,&pool));
        VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        da.descriptorPool=pool; da.descriptorSetCount=1; da.pSetLayouts=&descriptors;
        VkDescriptorSet set{}; VK(vkAllocateDescriptorSets(e.device,&da,&set));
        VkDescriptorImageInfo input{nullptr,color.view,VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet=set; write.dstBinding=0; write.descriptorCount=1; write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; write.pImageInfo=&input;
        vkUpdateDescriptorSets(e.device,1,&write,0,nullptr);
        constexpr unsigned samples=64;
        VkBufferCreateInfo bc{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bc.size=samples*8; bc.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VkBuffer readback{}; VK(vkCreateBuffer(e.device,&bc,nullptr,&readback));
        VkMemoryRequirements requirements{}; vkGetBufferMemoryRequirements(e.device,readback,&requirements);
        VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ma.allocationSize=requirements.size;
        ma.memoryTypeIndex=e.memoryType(requirements.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkDeviceMemory memory{}; VK(vkAllocateMemory(e.device,&ma,nullptr,&memory));
        VK(vkBindBufferMemory(e.device,readback,memory,0));
        void* mapped{}; VK(vkMapMemory(e.device,memory,0,VK_WHOLE_SIZE,0,&mapped));
        VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fence{}; VK(vkCreateFence(e.device,&fc,nullptr,&fence));
        auto producer=e.command(), consumer=e.command();
        std::array<VkBufferImageCopy,samples> copies{};
        for(unsigned i=0;i<samples;++i) {
            copies[i].bufferOffset=i*8;
            copies[i].imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
            copies[i].imageOffset={int(32+i*53),int(32+i*29),0};
            copies[i].imageExtent={1,1,1};
        }
        std::array<double,2> errors{};
        uint64_t value=0;
        for(unsigned candidate=0;candidate<2;++candidate) {
            const float sign=candidate==0?-1.0f:1.0f;
            BbMetalFX::TemporalScaler scaler(e.device,vkGetDeviceProcAddr,ready,done,{W,H,OW,OH});
            auto binding=scaler.Register({color.image,depth.image,motion.image,output.image,reactive.image});
            double error=0; unsigned count=0;
            for(unsigned frame=0;frame<16;++frame) {
                ++value;
                const float jx=(frame%2)?0.375f:-0.375f;
                const float jy=(frame%4<2)?0.25f:-0.25f;
                const float tx=frame*0.125f, ty=frame*0.0625f;
                begin(producer);
                for(auto* image:{&color,&depth,&motion,&reactive,&output})
                    barrier(producer,*image,value==1?VK_IMAGE_LAYOUT_UNDEFINED:VK_IMAGE_LAYOUT_GENERAL,
                        VK_IMAGE_LAYOUT_GENERAL,value==1?VK_QUEUE_FAMILY_IGNORED:VK_QUEUE_FAMILY_EXTERNAL,
                        value==1?VK_QUEUE_FAMILY_IGNORED:e.family);
                VkImageSubresourceRange dr{VK_IMAGE_ASPECT_DEPTH_BIT,0,1,0,1}, cr{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
                VkClearDepthStencilValue depthValue{0.5f,0};
                VkClearColorValue motionValue{}; motionValue.float32[0]=-0.125f; motionValue.float32[1]=-0.0625f;
                VkClearColorValue zero{};
                vkCmdClearDepthStencilImage(producer,depth.image,VK_IMAGE_LAYOUT_GENERAL,&depthValue,1,&dr);
                vkCmdClearColorImage(producer,motion.image,VK_IMAGE_LAYOUT_GENERAL,&motionValue,1,&cr);
                vkCmdClearColorImage(producer,reactive.image,VK_IMAGE_LAYOUT_GENERAL,&zero,1,&cr);
                vkCmdBindPipeline(producer,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
                vkCmdBindDescriptorSets(producer,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&set,0,nullptr);
                const std::array<float,4> params{jx,jy,tx,ty};
                vkCmdPushConstants(producer,layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(params),params.data());
                vkCmdDispatch(producer,(W+7)/8,(H+7)/8,1);
                for(auto* image:{&color,&depth,&motion,&reactive,&output})
                    barrier(producer,*image,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL,e.family,VK_QUEUE_FAMILY_EXTERNAL);
                submit(e,producer,nullptr,0,ready,value,nullptr);
                scaler.Submit(binding,{value,value,frame,0,sign*jx,sign*jy});
                begin(consumer);
                barrier(consumer,output,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_QUEUE_FAMILY_EXTERNAL,e.family);
                vkCmdCopyImageToBuffer(consumer,output.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback,samples,copies.data());
                barrier(consumer,output,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL,e.family,VK_QUEUE_FAMILY_EXTERNAL);
                submit(e,consumer,done,value,nullptr,0,fence); complete(e,fence); scaler.Drain();
                if(frame>=8) for(unsigned i=0;i<samples;++i) {
                    _Float16 rgb[4]; std::memcpy(rgb,static_cast<const char*>(mapped)+i*8,8);
                    const double px=(copies[i].imageOffset.x+0.5)*double(W)/OW-tx;
                    const double py=(copies[i].imageOffset.y+0.5)*double(H)/OH-ty;
                    const double r=0.5+0.25*std::sin(px*0.3926990817), g=0.5+0.25*std::sin(py*0.3926990817);
                    require(std::isfinite(float(rgb[0])) && std::isfinite(float(rgb[1])),"jitter output is not finite");
                    error+=std::abs(float(rgb[0])-r)+std::abs(float(rgb[1])-g); count+=2;
                }
            }
            errors[candidate]=error/count;
            std::printf("Sampling jitter sign=%+.0f viewport shift; moving-grid MAE=%.8f\n",sign,errors[candidate]);
        }
        require(errors[0]<0.006 && errors[1]>errors[0]*2,"negative viewport jitter did not uniquely match the analytic reference");
        std::puts("PASS: MetalFX sampling jitter = negative Vulkan viewport shift; previous-current pixel motion");
        return 0;
    }} catch(const std::exception& ex) { std::fprintf(stderr,"MetalFX jitter qualification: %s\n",ex.what()); return 1; }
}
