// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <memory>
#include <vulkan/vulkan_core.h>

namespace BbMetalFX {

struct Config {
    /// Size of the color, depth, motion and reactive textures, and the exact output size.
    uint32_t input_width, input_height, output_width, output_height;
    bool depth_stencil = false;
    uint32_t max_frames_in_flight = 4;
    /// Linear HDR at the HDR hook: R16G16B16A16_SFLOAT with auto exposure. The finished,
    /// tonemapped frame of the scaled presets: R8G8B8A8_UNORM or _SRGB without auto exposure
    /// (it already carries its exposure). docs/macos/metalfx.md, placement per preset.
    VkFormat color_format = VK_FORMAT_R16G16B16A16_SFLOAT;
    /// R16G16B16A16_SFLOAT or R8G8B8A8_UNORM.
    VkFormat output_format = VK_FORMAT_R16G16B16A16_SFLOAT;
    bool auto_exposure = true;
    /// The frame's extent: the top-left of the input textures (aligned targets larger than the
    /// render resolution). 0 means the whole texture. The scale factor is output / content.
    uint32_t content_width = 0, content_height = 0;
};

struct Images {
    VkImage color, depth, motion, output, reactive;
};

struct Frame {
    // Vulkan signals producer_value before this encode; waits on consumer_value afterwards.
    uint64_t producer_value, consumer_value;
    uint64_t frame_id, history_epoch;
    // Render-pixel jitter, X right/Y down; matches the Vulkan viewport shift.
    float sampling_jitter_x, sampling_jitter_y;
    float pre_exposure = 1.0f;
};

class Textures {
    struct Impl;
    std::shared_ptr<Impl> impl;
    explicit Textures(std::shared_ptr<Impl> value);
    friend class TemporalScaler;
public:
    ~Textures();
};

// Apple-only temporal provider (linear HDR or tonemapped LDR input, Config). Vulkan owns images/semaphores and their lifetime.
// The caller performs external ownership transfers and retires the Vulkan consumer
// before destroying any input. Output RGB must be composed with the game's own alpha.
class TemporalScaler {
    struct Impl;
    std::unique_ptr<Impl> impl;
public:
    TemporalScaler(VkDevice device, PFN_vkGetDeviceProcAddr get_proc,
                   VkSemaphore producer, VkSemaphore consumer, Config config);
    ~TemporalScaler();
    TemporalScaler(const TemporalScaler&) = delete;
    TemporalScaler& operator=(const TemporalScaler&) = delete;
    // Export/validate once when images change; retain this binding across frames.
    std::shared_ptr<Textures> Register(Images images);
    void Submit(const std::shared_ptr<Textures>& textures, Frame frame);
    // Only for resize/provider destruction or tests, never the normal frame path.
    void Drain();
    double LastGpuMilliseconds() const;
};

} // namespace BbMetalFX
