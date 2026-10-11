// SPDX-License-Identifier: GPL-2.0-or-later
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>
#import <Security/Security.h>
#define VK_USE_PLATFORM_METAL_EXT
#include "macos_metalfx.h"
#include <vulkan/vulkan_metal.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <thread>

namespace BbMetalFX {
namespace {
void Require(bool condition, const char* feature) {
    if (!condition) {
        std::fprintf(stderr, "MetalFX temporal: %s\n", feature);
        std::abort();
    }
}
void RequireSignedHost() {
    SecCodeRef code = nullptr;
    const OSStatus copied = SecCodeCopySelf(kSecCSDefaultFlags, &code);
    Require(copied == errSecSuccess && code, "cannot inspect host code signature");
    const OSStatus valid = SecCodeCheckValidity(code, kSecCSStrictValidate, nullptr);
    CFRelease(code);
    Require(valid == errSecSuccess,
            "host signature invalid or missing; ad-hoc codesign is required for ANE model compilation");
}
void Completed(id<MTLCommandBuffer> command) {
    if (command.status == MTLCommandBufferStatusError) {
        std::fprintf(stderr, "MetalFX temporal: command failed: %s\n",
                     command.error.description.UTF8String);
        std::abort();
    }
}
} // namespace

struct Textures::Impl {
    id<MTLTexture> color, depth, motion, output, reactive;
    std::shared_ptr<const unsigned char> owner;
};
Textures::Textures(std::shared_ptr<Impl> value) : impl{std::move(value)} {}
Textures::~Textures() = default;

struct TemporalScaler::Impl {
    Config config;
    VkDevice device;
    PFN_vkExportMetalObjectsEXT export_metal;
    id<MTLDevice> metal;
    id<MTLCommandQueue> queue;
    id<MTLSharedEvent> producer, consumer;
    id<MTLFXTemporalScaler> scaler;
    /// Set while a public call is running: the provider may be driven from different threads
    /// over time (the scheduler's owner moves between the draw-pipe thread and its caller), but
    /// never from two at once.
    mutable std::atomic<bool> busy{false};
    MTLPixelFormat color_format = MTLPixelFormatRGBA16Float, output_format = MTLPixelFormatRGBA16Float;
    std::shared_ptr<const unsigned char> binding_owner = std::make_shared<const unsigned char>(0);
    std::deque<id<MTLCommandBuffer>> pending;
    uint64_t last_producer = 0, last_consumer = 0, last_frame = 0, last_epoch = 0;
    bool dispatched = false;
    double last_gpu_ms = 0;

    /// Held for the duration of a public call; a second, overlapping call aborts.
    class Exclusive {
        const Impl& impl;
    public:
        explicit Exclusive(const Impl& value) : impl{value} {
            Require(!impl.busy.exchange(true, std::memory_order_acquire),
                    "provider used from two threads at once");
        }
        ~Exclusive() { impl.busy.store(false, std::memory_order_release); }
    };
    /// Waits for every submitted MetalFX command buffer (thread-safe: waitUntilCompleted).
    void WaitAll();
    id<MTLTexture> ExportImage(VkImage image, VkImageAspectFlagBits aspect) const {
        Require(image != VK_NULL_HANDLE, "required color/depth/motion/output/reactive image is absent");
        VkExportMetalTextureInfoEXT texture{};
        texture.sType = VK_STRUCTURE_TYPE_EXPORT_METAL_TEXTURE_INFO_EXT;
        texture.image = image;
        texture.plane = aspect;
        VkExportMetalObjectsInfoEXT objects{};
        objects.sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT;
        objects.pNext = &texture;
        export_metal(device, &objects);
        Require(texture.mtlTexture != nil, "VK_EXT_metal_objects texture export failed");
        return texture.mtlTexture;
    }
    void Validate(id<MTLTexture> texture, NSUInteger width, NSUInteger height,
                  MTLPixelFormat format, MTLTextureUsage usage) const {
        Require(texture.device.registryID == metal.registryID,
                "exported texture belongs to another Metal device");
        Require(texture.textureType == MTLTextureType2D && texture.sampleCount == 1 &&
                texture.width == width && texture.height == height && texture.pixelFormat == format,
                "texture dimension, sample count or format violates the MetalFX input contract");
        Require((texture.usage & usage) == usage, "exported texture lacks required MetalFX usage flags");
    }
    void Reap() {
        while (!pending.empty()) {
            auto command = pending.front();
            Completed(command);
            if (command.status != MTLCommandBufferStatusCompleted) break;
            last_gpu_ms = (command.GPUEndTime - command.GPUStartTime) * 1000.0;
            pending.pop_front();
        }
    }
};

TemporalScaler::TemporalScaler(VkDevice device, PFN_vkGetDeviceProcAddr get_proc,
                               VkSemaphore producer, VkSemaphore consumer, Config config)
    : impl{std::make_unique<Impl>()} {
    @autoreleasepool {
        RequireSignedHost();
        Require(config.input_width && config.input_height && config.output_width &&
                config.output_height && config.max_frames_in_flight,
                "configuration contains an empty extent or in-flight capacity");
        Require(device && get_proc && producer && consumer && producer != consumer,
                "device and two distinct exportable timeline semaphores are required");
        impl->config = config;
        impl->device = device;
        impl->export_metal = reinterpret_cast<PFN_vkExportMetalObjectsEXT>(
            get_proc(device, "vkExportMetalObjectsEXT"));
        Require(impl->export_metal, "VK_EXT_metal_objects is required");
        VkExportMetalSharedEventInfoEXT ready{}, done{};
        ready.sType = done.sType = VK_STRUCTURE_TYPE_EXPORT_METAL_SHARED_EVENT_INFO_EXT;
        ready.semaphore = producer;
        done.semaphore = consumer;
        ready.pNext = &done;
        VkExportMetalDeviceInfoEXT metal{};
        metal.sType = VK_STRUCTURE_TYPE_EXPORT_METAL_DEVICE_INFO_EXT;
        metal.pNext = &ready;
        VkExportMetalObjectsInfoEXT objects{};
        objects.sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT;
        objects.pNext = &metal;
        impl->export_metal(device, &objects);
        impl->metal = metal.mtlDevice;
        impl->producer = ready.mtlSharedEvent;
        impl->consumer = done.mtlSharedEvent;
        Require(impl->metal && impl->producer && impl->consumer,
                "Metal device/shared-event export failed; declare export intent during Vulkan creation");
        Require([MTLFXTemporalScalerDescriptor supportsDevice:impl->metal],
                "temporal scaling is unsupported by this device");
        const uint32_t content_width = config.content_width ? config.content_width : config.input_width;
        const uint32_t content_height = config.content_height ? config.content_height : config.input_height;
        Require(content_width <= config.input_width && content_height <= config.input_height,
                "content extent exceeds the input textures");
        const double sx = double(config.output_width) / content_width;
        const double sy = double(config.output_height) / content_height;
        const double low = [MTLFXTemporalScalerDescriptor supportedInputContentMinScaleForDevice:impl->metal];
        const double high = [MTLFXTemporalScalerDescriptor supportedInputContentMaxScaleForDevice:impl->metal];
        Require(std::abs(sx - sy) < 0.001 && sx >= low && sx <= high,
                "unsupported scale factor or changed aspect ratio");
        switch (config.color_format) {
        case VK_FORMAT_R16G16B16A16_SFLOAT: impl->color_format = MTLPixelFormatRGBA16Float; break;
        case VK_FORMAT_R8G8B8A8_UNORM: impl->color_format = MTLPixelFormatRGBA8Unorm; break;
        case VK_FORMAT_R8G8B8A8_SRGB: impl->color_format = MTLPixelFormatRGBA8Unorm_sRGB; break;
        default: Require(false, "color format is not RGBA16F, RGBA8 UNORM or RGBA8 sRGB");
        }
        switch (config.output_format) {
        case VK_FORMAT_R16G16B16A16_SFLOAT: impl->output_format = MTLPixelFormatRGBA16Float; break;
        case VK_FORMAT_R8G8B8A8_UNORM: impl->output_format = MTLPixelFormatRGBA8Unorm; break;
        default: Require(false, "output format is not RGBA16F or RGBA8 UNORM");
        }
        // A tonemapped frame already carries its exposure; MetalFX's own would expose it twice.
        Require(!config.auto_exposure || config.color_format == VK_FORMAT_R16G16B16A16_SFLOAT,
                "auto exposure on a tonemapped RGBA8 input");
        auto descriptor = [MTLFXTemporalScalerDescriptor new];
        descriptor.inputWidth = config.input_width;
        descriptor.inputHeight = config.input_height;
        descriptor.outputWidth = config.output_width;
        descriptor.outputHeight = config.output_height;
        descriptor.colorTextureFormat = impl->color_format;
        descriptor.depthTextureFormat = config.depth_stencil ? MTLPixelFormatDepth32Float_Stencil8
                                                           : MTLPixelFormatDepth32Float;
        descriptor.motionTextureFormat = MTLPixelFormatRG16Float;
        descriptor.outputTextureFormat = impl->output_format;
        descriptor.reactiveMaskTextureEnabled = YES;
        descriptor.reactiveMaskTextureFormat = MTLPixelFormatR8Unorm;
        descriptor.autoExposureEnabled = config.auto_exposure ? YES : NO;
        descriptor.requiresSynchronousInitialization = YES;
        impl->scaler = [descriptor newTemporalScalerWithDevice:impl->metal];
        Require(impl->scaler, "synchronous temporal-scaler creation failed");
        impl->queue = [impl->metal newCommandQueue];
        Require(impl->queue, "Metal command-queue creation failed");
        impl->queue.label = @"bbport MetalFX temporal";
        impl->scaler.motionVectorScaleX = impl->scaler.motionVectorScaleY = 1.0f;
        impl->scaler.depthReversed = NO;
        impl->scaler.inputContentWidth = content_width;
        impl->scaler.inputContentHeight = content_height;
    }
}

// The destructor is the scaler's last user, so it may run on any thread (the renderer may be torn
// down after the encoding thread has stopped): it waits for the pending work.
TemporalScaler::~TemporalScaler() {
    @autoreleasepool {
        impl->WaitAll();
    }
}

std::shared_ptr<Textures> TemporalScaler::Register(Images images) {
    @autoreleasepool {
        const Impl::Exclusive exclusive{*impl};
        auto textures = std::make_shared<Textures::Impl>();
        textures->owner = impl->binding_owner;
        textures->color = impl->ExportImage(images.color, VK_IMAGE_ASPECT_COLOR_BIT);
        textures->depth = impl->ExportImage(images.depth, VK_IMAGE_ASPECT_DEPTH_BIT);
        textures->motion = impl->ExportImage(images.motion, VK_IMAGE_ASPECT_COLOR_BIT);
        textures->output = impl->ExportImage(images.output, VK_IMAGE_ASPECT_COLOR_BIT);
        textures->reactive = impl->ExportImage(images.reactive, VK_IMAGE_ASPECT_COLOR_BIT);
        const auto& config = impl->config;
        const auto tw = config.input_width, th = config.input_height;
        impl->Validate(textures->color, tw, th, impl->color_format, impl->scaler.colorTextureUsage);
        impl->Validate(textures->depth, tw, th, impl->scaler.depthTextureFormat,
                       impl->scaler.depthTextureUsage);
        impl->Validate(textures->motion, tw, th, MTLPixelFormatRG16Float, impl->scaler.motionTextureUsage);
        impl->Validate(textures->reactive, tw, th, MTLPixelFormatR8Unorm, impl->scaler.reactiveTextureUsage);
        impl->Validate(textures->output, config.output_width, config.output_height,
                       impl->output_format, impl->scaler.outputTextureUsage);
        Require(textures->output.storageMode == MTLStorageModePrivate,
                "MetalFX output must use private storage");
        Require(textures->output != textures->color && textures->output != textures->depth &&
                textures->output != textures->motion && textures->output != textures->reactive,
                "MetalFX output aliases an input");
        return std::shared_ptr<Textures>(new Textures(std::move(textures)));
    }
}

void TemporalScaler::Submit(const std::shared_ptr<Textures>& textures, Frame frame) {
    @autoreleasepool {
        const Impl::Exclusive exclusive{*impl};
        Require(textures && textures->impl->owner == impl->binding_owner,
                "binding belongs to another provider");
        Require(frame.producer_value > impl->last_producer && frame.consumer_value > impl->last_consumer,
                "shared-event values must increase for each temporal submission");
        Require(!impl->dispatched || frame.frame_id > impl->last_frame,
                "frame IDs must increase for each temporal submission");
        Require(std::isfinite(frame.sampling_jitter_x) && std::isfinite(frame.sampling_jitter_y) &&
                std::abs(frame.sampling_jitter_x) <= 0.5f && std::abs(frame.sampling_jitter_y) <= 0.5f &&
                std::isfinite(frame.pre_exposure) && frame.pre_exposure > 0,
                "invalid sampling jitter or pre-exposure");
        impl->Reap();
        Require(impl->pending.size() < impl->config.max_frames_in_flight,
                "in-flight capacity exceeded; Vulkan scheduler must retire frames");
        auto scaler = impl->scaler;
        scaler.colorTexture = textures->impl->color;
        scaler.depthTexture = textures->impl->depth;
        scaler.motionTexture = textures->impl->motion;
        scaler.outputTexture = textures->impl->output;
        scaler.reactiveMaskTexture = textures->impl->reactive;
        scaler.jitterOffsetX = frame.sampling_jitter_x;
        scaler.jitterOffsetY = frame.sampling_jitter_y;
        scaler.preExposure = frame.pre_exposure;
        scaler.reset = !impl->dispatched || frame.frame_id != impl->last_frame + 1 ||
                       frame.history_epoch != impl->last_epoch;
        auto command = [impl->queue commandBuffer];
        Require(command, "Metal command-buffer allocation failed");
        [command encodeWaitForEvent:impl->producer value:frame.producer_value];
        [scaler encodeToCommandBuffer:command];
        [command encodeSignalEvent:impl->consumer value:frame.consumer_value];
        // Keep the exported textures alive until Metal execution retires.
        const auto retained_textures = textures;
        [command addCompletedHandler:^(id<MTLCommandBuffer> completed) {
            (void)retained_textures;
            Completed(completed);
        }];
        impl->pending.push_back(command);
        [command commit];
        impl->last_producer = frame.producer_value;
        impl->last_consumer = frame.consumer_value;
        impl->last_frame = frame.frame_id;
        impl->last_epoch = frame.history_epoch;
        impl->dispatched = true;
    }
}

void TemporalScaler::Impl::WaitAll() {
    for (auto command : pending) {
        [command waitUntilCompleted];
        Completed(command);
    }
    Reap();
}

void TemporalScaler::Drain() {
    @autoreleasepool {
        const Impl::Exclusive exclusive{*impl};
        impl->WaitAll();
    }
}
double TemporalScaler::LastGpuMilliseconds() const {
    const Impl::Exclusive exclusive{*impl};
    return impl->last_gpu_ms;
}
} // namespace BbMetalFX
