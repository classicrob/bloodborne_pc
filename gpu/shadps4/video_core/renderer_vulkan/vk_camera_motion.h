// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: camera motion vectors for temporal upscaling (docs/upscaler.md). The scene constants
// give the camera of each frame; the previous one is kept here. BB_DEBUG_MOTION=1 blends the
// motion vectors as colors into the frame before it is copied to the display.

#pragma once

#include <array>
#include <cmath>
#include <optional>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/texture_cache/image.h"

namespace VideoCore {
class TextureCache;
}

namespace Vulkan {

/// One frame's scene viewport, from its G-buffer draws ({x scale, y scale, x offset, y offset},
/// render pixels, without jitter: pixel = offset + ndc * scale). Usable only when every G-buffer
/// draw of the frame had the same finite viewport with nonzero scales; a draw without one, a
/// non-finite or zero scale, or a second different viewport makes the frame unusable until
/// Reset (the next frame's camera). Then the frame has no camera motion: no single mapping
/// inverts depth written through several.
class SceneViewportLatch {
public:
    using Viewport = std::array<float, 4>;
    enum class State : u8 { Unset, Usable, Unusable };
    /// What a Note did, for one-time reports.
    enum class Note : u8 { First, Same, Missing, Invalid, Differs, AlreadyUnusable };

    void Reset() noexcept {
        state = State::Unset;
        viewport = {};
    }
    Note Add(const std::optional<Viewport>& next) noexcept;
    [[nodiscard]] bool Usable() const noexcept {
        return state == State::Usable;
    }
    [[nodiscard]] State Get() const noexcept {
        return state;
    }
    /// The frame's viewport; zero unless Usable.
    [[nodiscard]] const Viewport& Value() const noexcept {
        return viewport;
    }

private:
    State state = State::Unset;
    Viewport viewport{};
};

inline SceneViewportLatch::Note SceneViewportLatch::Add(
    const std::optional<Viewport>& next) noexcept {
    if (state == State::Unusable) {
        return Note::AlreadyUnusable;
    }
    const auto unusable = [this](Note note) {
        state = State::Unusable;
        viewport = {};
        return note;
    };
    if (!next) {
        return unusable(Note::Missing);
    }
    const Viewport& v = *next;
    if (!std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2]) ||
        !std::isfinite(v[3]) || v[0] == 0.0f || v[1] == 0.0f) {
        return unusable(Note::Invalid);
    }
    if (state == State::Unset) {
        state = State::Usable;
        viewport = v;
        return Note::First;
    }
    return v == viewport ? Note::Same : unusable(Note::Differs);
}

/// Camera motion (the upscaler's vectors, TAA's depth check, the debug overlay) needs both
/// cameras, both frames' scene viewports and the current scene depth.
[[nodiscard]] inline bool CameraMotionReady(bool current_camera, bool previous_camera,
                                            const SceneViewportLatch& current_viewport,
                                            const SceneViewportLatch& previous_viewport,
                                            bool depth) noexcept {
    return current_camera && previous_camera && current_viewport.Usable() &&
           previous_viewport.Usable() && depth;
}

class Instance;
class Scheduler;
class Runtime;
class ObjectMotion;

class CameraMotion {
public:
    CameraMotion(const Instance& instance, Scheduler& scheduler,
                 VideoCore::TextureCache& texture_cache, Runtime& runtime);
    ~CameraMotion();
    void InvalidateHistory() { current.valid = previous.valid = false; }
    void SetObjectMotion(ObjectMotion* motion) noexcept {
        object_motion = motion;
    }
    /// This frame's object motion image (after RecordMotion), for the debug view; or null.
    vk::ImageView ObjectMotionView() const noexcept;
    vk::Image ObjectMotionImage(u32 width, u32 height) const noexcept;

    void SetJitter(std::array<float, 2> value) noexcept {
        previous_jitter = jitter;
        jitter = value;
    }

    /// What the last RecordMotion used, for dump provenance (tools/macos/motion_agree.py).
    struct MotionRecord {
        float proj_z = 0;          ///< current projection z scale (the shader's depth threshold)
        u32 mode = 0;              ///< 1: object vectors merged in; 0: camera vectors only
        bool camera_only = false;  ///< BB_MOTION_CAMERA_ONLY
        bool object_valid = false; ///< an object motion image was available
        std::array<float, 4> viewport{};      ///< current scene viewport (SceneViewportLatch)
        std::array<float, 4> prev_viewport{}; ///< previous frame's
    };
    [[nodiscard]] MotionRecord LastRecord() const noexcept {
        return last_record;
    }

    [[nodiscard]] bool Enabled() const noexcept {
        return debug_overlay || for_upscaler;
    }

    /// Both cameras, their scene viewports and the scene depth of the current frame are known.
    [[nodiscard]] bool Ready() const noexcept {
        return CameraMotionReady(current.valid, previous.valid, current.viewport,
                                 previous.viewport, bool(depth_id));
    }
    [[nodiscard]] VideoCore::ImageId Depth() const noexcept {
        return depth_id;
    }
    /// The render size in the scene constants (the game's viewport; its targets may be
    /// allocated larger, e.g. 1916x1080 for a 1916x1078 scene), or zero before the first camera.
    [[nodiscard]] std::array<u32, 2> RenderSize() const noexcept {
        return render_size;
    }
    /// Vertical field of view and near/far planes of the current camera.
    [[nodiscard]] float VerticalFov() const noexcept;
    [[nodiscard]] float Near() const noexcept;
    // Current projection, previous projection, row Z of previous-view * inverse-view, and the
    // current scene viewport.
    [[nodiscard]] std::array<std::array<float, 4>, 4> TaaDepthParameters() const noexcept;

    /// Records the motion vector pass: `depth_view` (depth aspect, General layout) into
    /// `motion_view` (RG16F storage, General), pixels, previous minus current.
    void RecordMotion(vk::CommandBuffer cmdbuf, vk::ImageView depth_view, vk::ImageView motion_view,
                      u32 width, u32 height);

    /// A bound constant buffer of 864 bytes: checks the scene constant signature.
    void OnConstants(const float* data);

    /// The G-buffer pass (5+ color targets): its depth is the scene depth.
    void OnGBufferPass(VideoCore::ImageId depth);

    /// A G-buffer draw's effective viewport in render pixels, without the upscaler's jitter:
    /// pixel = offset + ndc * scale, as {x scale, y scale, x offset, y offset}, signs as the game
    /// set them; nullopt when the draw has none (clip disabled, a zero scale). The camera motion
    /// inverts the frame's one viewport (SceneViewportLatch; the object vectors use the same
    /// mapping).
    using Viewport = SceneViewportLatch::Viewport;
    void OnGBufferViewport(const std::optional<Viewport>& viewport);

    /// The pass copying the finished frame (`frame`, the last target drawn) to the display: the
    /// frame's depth and camera are complete. Records the debug overlay, starts a new frame.
    void OnDisplayPass(VideoCore::ImageId frame);

private:
    struct Camera {
        std::array<float, 12> view{};     ///< world to view, 3x4 rows
        std::array<float, 12> inv_view{}; ///< view to world, 3x4 rows
        std::array<float, 4> proj{};      ///< x scale, y scale, z scale, z offset
        bool valid = false;
        SceneViewportLatch viewport; ///< the frame's scene pass viewport (OnGBufferViewport)
    };

    void Overlay(VideoCore::ImageId frame);

    const Instance& instance;
    Scheduler& scheduler;
    VideoCore::TextureCache& texture_cache;
    Runtime& runtime;
    bool debug_overlay = false;
    bool camera_only = false; ///< BB_MOTION_CAMERA_ONLY (diagnostic)
    MotionRecord last_record;
    bool for_upscaler = false;
    vk::UniqueDescriptorSetLayout motion_desc_layout;
    vk::UniquePipelineLayout motion_pipeline_layout;
    vk::UniquePipeline motion_pipeline;

    ObjectMotion* object_motion = nullptr;
    Camera current, previous;
    bool frame_has_camera = false;
    std::array<float, 2> jitter{}, previous_jitter{};
    std::array<u32, 2> render_size{};
    VideoCore::ImageId depth_id{};

    vk::UniqueDescriptorSetLayout desc_layout;
    vk::UniquePipelineLayout pipeline_layout;
    vk::UniquePipeline overlay_pipeline;
    /// Debug: the last two finished frames (RGBA8 packed), for the reprojection check.
    std::unique_ptr<VideoCore::Buffer> frames[2];
    u32 frame_index = 0;
};

} // namespace Vulkan
