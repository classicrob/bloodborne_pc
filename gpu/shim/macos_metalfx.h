// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <memory>
#include <vulkan/vulkan_core.h>

namespace BbMetalFX {

struct Config {
    uint32_t input_width, input_height, output_width, output_height;
    bool depth_stencil = false;
    uint32_t max_frames_in_flight = 4;
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

// Apple-only linear HDR provider. Vulkan owns images/semaphores and their lifetime.
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
