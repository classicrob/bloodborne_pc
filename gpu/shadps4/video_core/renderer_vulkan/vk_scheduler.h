// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once
#ifdef __APPLE__
#include "macos_perf.h"
#endif

#include <atomic>
#include <chrono>
#include <cstring>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <span>
#include <utility>
#include <vector>
#include <mutex>
#ifdef __APPLE__
#include "bbport_deferred_counts.h"
#include "bbport_label_sequencer.h"
#endif
#include <thread>
#include <queue>

#include "bbport_toggles.h"
#include "common/assert.h"
#ifdef __APPLE__
#include "bbport_pass_census.h"
#endif
#include "common/interval_set.h"
#include "common/unique_function.h"
#include "video_core/amdgpu/regs_color.h"
#include "video_core/amdgpu/regs_primitive.h"
#include "video_core/renderer_vulkan/vk_resource_pool.h"
#include "video_core/renderer_vulkan/vk_semaphore.h"

namespace tracy {
class VkCtxScope;
}

namespace Vulkan {

class Instance;

struct RenderAttachment {
    vk::ImageView image_view;
    vk::ImageLayout image_layout;
    std::array<u32, 4> clear_value;
    union {
        u32 is_clear;
        struct {
            bool has_depth;
            bool depth_clear;
            bool has_stencil;
            bool stencil_clear;
        };
    };
};
static_assert(std::has_unique_object_representations_v<RenderAttachment>);

struct RenderState {
    std::array<RenderAttachment, 8> color_attachments;
    RenderAttachment depth_stencil_attachment;
    u16 width;
    u16 height;
    u16 num_layers;
    u16 num_color_attachments;

    bool operator==(const RenderState& other) const noexcept {
        return std::memcmp(this, &other, sizeof(RenderState)) == 0;
    }
};
static_assert(std::has_unique_object_representations_v<RenderState>);

struct SubmitInfo {
    std::array<vk::Semaphore, 4> wait_semas;
    std::array<u64, 4> wait_ticks;
    /// Stages each wait blocks (AllCommands unless the caller says otherwise).
    std::array<vk::PipelineStageFlags, 4> wait_stages;
    std::array<vk::Semaphore, 4> signal_semas;
    std::array<u64, 4> signal_ticks;
    vk::Fence fence;
    u32 num_wait_semas;
    u32 num_signal_semas;

    void AddWait(vk::Semaphore semaphore, u64 tick = 1,
                 vk::PipelineStageFlags stage = vk::PipelineStageFlagBits::eAllCommands) {
        ASSERT_MSG(num_wait_semas < wait_semas.size(), "SubmitInfo holds at most {} waits",
                   wait_semas.size());
        wait_semas[num_wait_semas] = semaphore;
        wait_stages[num_wait_semas] = stage;
        wait_ticks[num_wait_semas++] = tick;
    }

    void AddSignal(vk::Semaphore semaphore, u64 tick = 1) {
        ASSERT_MSG(num_signal_semas < signal_semas.size(), "SubmitInfo holds at most {} signals",
                   signal_semas.size());
        signal_semas[num_signal_semas] = semaphore;
        signal_ticks[num_signal_semas++] = tick;
    }

    void AddSignal(vk::Fence fence) {
        this->fence = fence;
    }
};

using Viewports = boost::container::static_vector<vk::Viewport, AmdGpu::NUM_VIEWPORTS>;
using Scissors = boost::container::static_vector<vk::Rect2D, AmdGpu::NUM_VIEWPORTS>;
using ColorWriteMasks = std::array<vk::ColorComponentFlags, AmdGpu::NUM_COLOR_BUFFERS>;
struct StencilOps {
    vk::StencilOp fail_op{};
    vk::StencilOp pass_op{};
    vk::StencilOp depth_fail_op{};
    vk::CompareOp compare_op{};

    bool operator==(const StencilOps& other) const {
        return fail_op == other.fail_op && pass_op == other.pass_op &&
               depth_fail_op == other.depth_fail_op && compare_op == other.compare_op;
    }
};
struct DynamicState {
    struct {
        bool viewports : 1;
        bool scissors : 1;

        bool depth_test_enabled : 1;
        bool depth_write_enabled : 1;
        bool depth_compare_op : 1;

        bool depth_bounds_test_enabled : 1;
        bool depth_bounds : 1;

        bool depth_bias_enabled : 1;
        bool depth_bias : 1;

        bool stencil_test_enabled : 1;
        bool stencil_front_ops : 1;
        bool stencil_front_reference : 1;
        bool stencil_front_write_mask : 1;
        bool stencil_front_compare_mask : 1;
        bool stencil_back_ops : 1;
        bool stencil_back_reference : 1;
        bool stencil_back_write_mask : 1;
        bool stencil_back_compare_mask : 1;

        bool primitive_restart_enable : 1;
        bool rasterizer_discard_enable : 1;
        bool cull_mode : 1;
        bool front_face : 1;

        bool blend_constants : 1;
        bool color_write_masks : 1;
        bool line_width : 1;
        bool feedback_loop_enabled : 1;
    } dirty_state{};

    Viewports viewports{};
    Scissors scissors{};

    bool depth_test_enabled{};
    bool depth_write_enabled{};
    vk::CompareOp depth_compare_op{};

    bool depth_bounds_test_enabled{};
    float depth_bounds_min{};
    float depth_bounds_max{};

    bool depth_bias_enabled{};
    float depth_bias_constant{};
    float depth_bias_clamp{};
    float depth_bias_slope{};

    bool stencil_test_enabled{};
    StencilOps stencil_front_ops{};
    u32 stencil_front_reference{};
    u32 stencil_front_write_mask{};
    u32 stencil_front_compare_mask{};
    StencilOps stencil_back_ops{};
    u32 stencil_back_reference{};
    u32 stencil_back_write_mask{};
    u32 stencil_back_compare_mask{};

    bool primitive_restart_enable{};
    bool rasterizer_discard_enable{};
    vk::CullModeFlags cull_mode{};
    vk::FrontFace front_face{};

    std::array<float, 4> blend_constants{};
    ColorWriteMasks color_write_masks{};
    float line_width{};
    bool feedback_loop_enabled{};

    /// Commits the dynamic state to the provided command buffer (a null one only clears the
    /// flags that committing clears).
    void Commit(const Instance& instance, const vk::CommandBuffer& cmdbuf);

    /// bbport: passes each dirty state change to `emit` as a small command closure holding only
    /// its own values (recording a copy of the whole state per draw was a hot spot), and clears
    /// the flags it emitted. Flags whose test is disabled stay dirty, as in Commit.
    template <typename Emit>
    void CommitWith(bool depth_bounds_supported, bool color_write_mask_supported,
                    bool feedback_loop_supported, Emit&& emit) {
        using Face = vk::StencilFaceFlagBits;
        if (dirty_state.viewports) {
            dirty_state.viewports = false;
            emit([v = viewports](vk::CommandBuffer c) { c.setViewportWithCount(v); });
        }
        if (dirty_state.scissors) {
            dirty_state.scissors = false;
            emit([v = scissors](vk::CommandBuffer c) { c.setScissorWithCount(v); });
        }
        if (dirty_state.depth_test_enabled) {
            dirty_state.depth_test_enabled = false;
            emit([v = depth_test_enabled](vk::CommandBuffer c) { c.setDepthTestEnable(v); });
        }
        if (dirty_state.depth_write_enabled) {
            // Must be set in a command buffer even if depth test is disabled.
            dirty_state.depth_write_enabled = false;
            emit([v = depth_write_enabled](vk::CommandBuffer c) { c.setDepthWriteEnable(v); });
        }
        if (depth_test_enabled && dirty_state.depth_compare_op) {
            dirty_state.depth_compare_op = false;
            emit([v = depth_compare_op](vk::CommandBuffer c) { c.setDepthCompareOp(v); });
        }
        if (dirty_state.depth_bounds_test_enabled) {
            dirty_state.depth_bounds_test_enabled = false;
            if (depth_bounds_supported) {
                emit([v = depth_bounds_test_enabled](vk::CommandBuffer c) {
                    c.setDepthBoundsTestEnable(v);
                });
            }
        }
        if (depth_bounds_test_enabled && dirty_state.depth_bounds) {
            dirty_state.depth_bounds = false;
            if (depth_bounds_supported) {
                emit([lo = depth_bounds_min, hi = depth_bounds_max](vk::CommandBuffer c) {
                    c.setDepthBounds(lo, hi);
                });
            }
        }
        if (dirty_state.depth_bias_enabled) {
            dirty_state.depth_bias_enabled = false;
            emit([v = depth_bias_enabled](vk::CommandBuffer c) { c.setDepthBiasEnable(v); });
        }
        if (depth_bias_enabled && dirty_state.depth_bias) {
            dirty_state.depth_bias = false;
            emit([k = depth_bias_constant, clamp = depth_bias_clamp,
                  slope = depth_bias_slope](vk::CommandBuffer c) { c.setDepthBias(k, clamp, slope); });
        }
        if (dirty_state.stencil_test_enabled) {
            dirty_state.stencil_test_enabled = false;
            emit([v = stencil_test_enabled](vk::CommandBuffer c) { c.setStencilTestEnable(v); });
        }
        if (stencil_test_enabled) {
            const auto ops = [&](Face face, const StencilOps& o) {
                emit([face, o](vk::CommandBuffer c) {
                    c.setStencilOp(face, o.fail_op, o.pass_op, o.depth_fail_op, o.compare_op);
                });
            };
            if (dirty_state.stencil_front_ops && dirty_state.stencil_back_ops &&
                stencil_front_ops == stencil_back_ops) {
                ops(Face::eFrontAndBack, stencil_front_ops);
            } else {
                if (dirty_state.stencil_front_ops) {
                    ops(Face::eFront, stencil_front_ops);
                }
                if (dirty_state.stencil_back_ops) {
                    ops(Face::eBack, stencil_back_ops);
                }
            }
            dirty_state.stencil_front_ops = dirty_state.stencil_back_ops = false;

            const auto reference = [&](Face face, u32 v) {
                emit([face, v](vk::CommandBuffer c) { c.setStencilReference(face, v); });
            };
            if (dirty_state.stencil_front_reference && dirty_state.stencil_back_reference &&
                stencil_front_reference == stencil_back_reference) {
                reference(Face::eFrontAndBack, stencil_front_reference);
            } else {
                if (dirty_state.stencil_front_reference) {
                    reference(Face::eFront, stencil_front_reference);
                }
                if (dirty_state.stencil_back_reference) {
                    reference(Face::eBack, stencil_back_reference);
                }
            }
            dirty_state.stencil_front_reference = dirty_state.stencil_back_reference = false;

            const auto write_mask = [&](Face face, u32 v) {
                emit([face, v](vk::CommandBuffer c) { c.setStencilWriteMask(face, v); });
            };
            if (dirty_state.stencil_front_write_mask && dirty_state.stencil_back_write_mask &&
                stencil_front_write_mask == stencil_back_write_mask) {
                write_mask(Face::eFrontAndBack, stencil_front_write_mask);
            } else {
                if (dirty_state.stencil_front_write_mask) {
                    write_mask(Face::eFront, stencil_front_write_mask);
                }
                if (dirty_state.stencil_back_write_mask) {
                    write_mask(Face::eBack, stencil_back_write_mask);
                }
            }
            dirty_state.stencil_front_write_mask = dirty_state.stencil_back_write_mask = false;

            const auto compare_mask = [&](Face face, u32 v) {
                emit([face, v](vk::CommandBuffer c) { c.setStencilCompareMask(face, v); });
            };
            if (dirty_state.stencil_front_compare_mask && dirty_state.stencil_back_compare_mask &&
                stencil_front_compare_mask == stencil_back_compare_mask) {
                compare_mask(Face::eFrontAndBack, stencil_front_compare_mask);
            } else {
                if (dirty_state.stencil_front_compare_mask) {
                    compare_mask(Face::eFront, stencil_front_compare_mask);
                }
                if (dirty_state.stencil_back_compare_mask) {
                    compare_mask(Face::eBack, stencil_back_compare_mask);
                }
            }
            dirty_state.stencil_front_compare_mask = dirty_state.stencil_back_compare_mask = false;
        }
        if (dirty_state.primitive_restart_enable) {
            dirty_state.primitive_restart_enable = false;
            emit([v = primitive_restart_enable](vk::CommandBuffer c) {
                c.setPrimitiveRestartEnable(v);
            });
        }
        if (dirty_state.rasterizer_discard_enable) {
            dirty_state.rasterizer_discard_enable = false;
            emit([v = rasterizer_discard_enable](vk::CommandBuffer c) {
                c.setRasterizerDiscardEnable(v);
            });
        }
        if (dirty_state.cull_mode) {
            dirty_state.cull_mode = false;
            emit([v = cull_mode](vk::CommandBuffer c) { c.setCullMode(v); });
        }
        if (dirty_state.front_face) {
            dirty_state.front_face = false;
            emit([v = front_face](vk::CommandBuffer c) { c.setFrontFace(v); });
        }
        if (dirty_state.blend_constants) {
            dirty_state.blend_constants = false;
            emit([v = blend_constants](vk::CommandBuffer c) { c.setBlendConstants(v.data()); });
        }
        if (dirty_state.color_write_masks) {
            dirty_state.color_write_masks = false;
            if (color_write_mask_supported) {
                emit([v = color_write_masks](vk::CommandBuffer c) { c.setColorWriteMaskEXT(0, v); });
            }
        }
        if (dirty_state.line_width) {
            dirty_state.line_width = false;
            emit([v = line_width](vk::CommandBuffer c) { c.setLineWidth(v); });
        }
        if (dirty_state.feedback_loop_enabled && feedback_loop_supported) {
            dirty_state.feedback_loop_enabled = false;
            emit([v = feedback_loop_enabled](vk::CommandBuffer c) {
                c.setAttachmentFeedbackLoopEnableEXT(v ? vk::ImageAspectFlagBits::eColor
                                                       : vk::ImageAspectFlagBits::eNone);
            });
        }
    }

    /// bbport: true when Commit() would record anything. Flags that Commit() defers (their
    /// test is disabled) do not count.
    [[nodiscard]] bool AnyDirty() const noexcept {
        auto pending = dirty_state;
        if (!depth_test_enabled) {
            pending.depth_compare_op = false;
        }
        if (!depth_bounds_test_enabled) {
            pending.depth_bounds = false;
        }
        if (!depth_bias_enabled) {
            pending.depth_bias = false;
        }
        if (!stencil_test_enabled) {
            pending.stencil_front_ops = pending.stencil_back_ops = false;
            pending.stencil_front_reference = pending.stencil_back_reference = false;
            pending.stencil_front_write_mask = pending.stencil_back_write_mask = false;
            pending.stencil_front_compare_mask = pending.stencil_back_compare_mask = false;
        }
        static constexpr decltype(dirty_state) clean{};
        return std::memcmp(&pending, &clean, sizeof(pending)) != 0;
    }

    /// Invalidates all dynamic state to be flushed into the next command buffer.
    void Invalidate() {
        // bbport: named flags only; padding bits set by a memset made AnyDirty() always true.
        dirty_state.viewports = true;
        dirty_state.scissors = true;
        dirty_state.depth_test_enabled = true;
        dirty_state.depth_write_enabled = true;
        dirty_state.depth_compare_op = true;
        dirty_state.depth_bounds_test_enabled = true;
        dirty_state.depth_bounds = true;
        dirty_state.depth_bias_enabled = true;
        dirty_state.depth_bias = true;
        dirty_state.stencil_test_enabled = true;
        dirty_state.stencil_front_ops = true;
        dirty_state.stencil_front_reference = true;
        dirty_state.stencil_front_write_mask = true;
        dirty_state.stencil_front_compare_mask = true;
        dirty_state.stencil_back_ops = true;
        dirty_state.stencil_back_reference = true;
        dirty_state.stencil_back_write_mask = true;
        dirty_state.stencil_back_compare_mask = true;
        dirty_state.primitive_restart_enable = true;
        dirty_state.rasterizer_discard_enable = true;
        dirty_state.cull_mode = true;
        dirty_state.front_face = true;
        dirty_state.blend_constants = true;
        dirty_state.color_write_masks = true;
        dirty_state.line_width = true;
        dirty_state.feedback_loop_enabled = true;
    }

    void SetViewports(const Viewports& viewports_) {
        if (!std::ranges::equal(viewports, viewports_)) {
            viewports = viewports_;
            dirty_state.viewports = true;
        }
    }

    void SetScissors(const Scissors& scissors_) {
        if (!std::ranges::equal(scissors, scissors_)) {
            scissors = scissors_;
            dirty_state.scissors = true;
        }
    }

    void SetDepthTestEnabled(const bool enabled) {
        if (depth_test_enabled != enabled) {
            depth_test_enabled = enabled;
            dirty_state.depth_test_enabled = true;
        }
    }

    void SetDepthWriteEnabled(const bool enabled) {
        if (depth_write_enabled != enabled) {
            depth_write_enabled = enabled;
            dirty_state.depth_write_enabled = true;
        }
    }

    void SetDepthCompareOp(const vk::CompareOp compare_op) {
        if (depth_compare_op != compare_op) {
            depth_compare_op = compare_op;
            dirty_state.depth_compare_op = true;
        }
    }

    void SetDepthBoundsTestEnabled(const bool enabled) {
        if (depth_bounds_test_enabled != enabled) {
            depth_bounds_test_enabled = enabled;
            dirty_state.depth_bounds_test_enabled = true;
        }
    }

    void SetDepthBounds(const float min, const float max) {
        if (depth_bounds_min != min || depth_bounds_max != max) {
            depth_bounds_min = min;
            depth_bounds_max = max;
            dirty_state.depth_bounds = true;
        }
    }

    void SetDepthBiasEnabled(const bool enabled) {
        if (depth_bias_enabled != enabled) {
            depth_bias_enabled = enabled;
            dirty_state.depth_bias_enabled = true;
        }
    }

    void SetDepthBias(const float constant, const float clamp, const float slope) {
        if (depth_bias_constant != constant || depth_bias_clamp != clamp ||
            depth_bias_slope != slope) {
            depth_bias_constant = constant;
            depth_bias_clamp = clamp;
            depth_bias_slope = slope;
            dirty_state.depth_bias = true;
        }
    }

    void SetStencilTestEnabled(const bool enabled) {
        if (stencil_test_enabled != enabled) {
            stencil_test_enabled = enabled;
            dirty_state.stencil_test_enabled = true;
        }
    }

    void SetStencilOps(const StencilOps& front_ops, const StencilOps& back_ops) {
        if (stencil_front_ops != front_ops) {
            stencil_front_ops = front_ops;
            dirty_state.stencil_front_ops = true;
        }
        if (stencil_back_ops != back_ops) {
            stencil_back_ops = back_ops;
            dirty_state.stencil_back_ops = true;
        }
    }

    void SetStencilReferences(const u32 front_reference, const u32 back_reference) {
        if (stencil_front_reference != front_reference) {
            stencil_front_reference = front_reference;
            dirty_state.stencil_front_reference = true;
        }
        if (stencil_back_reference != back_reference) {
            stencil_back_reference = back_reference;
            dirty_state.stencil_back_reference = true;
        }
    }

    void SetStencilWriteMasks(const u32 front_write_mask, const u32 back_write_mask) {
        if (stencil_front_write_mask != front_write_mask) {
            stencil_front_write_mask = front_write_mask;
            dirty_state.stencil_front_write_mask = true;
        }
        if (stencil_back_write_mask != back_write_mask) {
            stencil_back_write_mask = back_write_mask;
            dirty_state.stencil_back_write_mask = true;
        }
    }

    void SetStencilCompareMasks(const u32 front_compare_mask, const u32 back_compare_mask) {
        if (stencil_front_compare_mask != front_compare_mask) {
            stencil_front_compare_mask = front_compare_mask;
            dirty_state.stencil_front_compare_mask = true;
        }
        if (stencil_back_compare_mask != back_compare_mask) {
            stencil_back_compare_mask = back_compare_mask;
            dirty_state.stencil_back_compare_mask = true;
        }
    }

    void SetPrimitiveRestartEnabled(const bool enabled) {
        if (primitive_restart_enable != enabled) {
            primitive_restart_enable = enabled;
            dirty_state.primitive_restart_enable = true;
        }
    }

    void SetCullMode(const vk::CullModeFlags cull_mode_) {
        if (cull_mode != cull_mode_) {
            cull_mode = cull_mode_;
            dirty_state.cull_mode = true;
        }
    }

    void SetFrontFace(const vk::FrontFace front_face_) {
        if (front_face != front_face_) {
            front_face = front_face_;
            dirty_state.front_face = true;
        }
    }

    void SetBlendConstants(const std::array<float, 4> blend_constants_) {
        if (blend_constants != blend_constants_) {
            blend_constants = blend_constants_;
            dirty_state.blend_constants = true;
        }
    }

    void SetRasterizerDiscardEnabled(const bool enabled) {
        if (rasterizer_discard_enable != enabled) {
            rasterizer_discard_enable = enabled;
            dirty_state.rasterizer_discard_enable = true;
        }
    }

    void SetColorWriteMasks(const ColorWriteMasks& color_write_masks_) {
        if (!std::ranges::equal(color_write_masks, color_write_masks_)) {
            color_write_masks = color_write_masks_;
            dirty_state.color_write_masks = true;
        }
    }

    void SetLineWidth(const float width) {
        if (line_width != width) {
            line_width = width;
            dirty_state.line_width = true;
        }
    }

    void SetAttachmentFeedbackLoopEnabled(const bool enabled) {
        if (feedback_loop_enabled != enabled) {
            feedback_loop_enabled = enabled;
            dirty_state.feedback_loop_enabled = true;
        }
    }
};

using SubmitFunc = Common::UniqueFunction<void, SubmitInfo&>;

/// bbport: a block of deferred Vulkan commands. Commands are closures placed in
/// fixed storage (no allocation per command) and run in order on the recording thread.
class RecordChunk {
public:
    static constexpr size_t Capacity = 128 * 1024;

    /// Returns false (and leaves `func` untouched) when the chunk has no room.
    template <typename Func>
    bool Push(Func&& func) {
        using Command = TypedCommand<std::decay_t<Func>>;
        static_assert(sizeof(Command) <= Capacity, "recorded command is too large");
        const size_t offset = (used + alignof(Command) - 1) & ~(alignof(Command) - 1);
        if (offset + sizeof(Command) > Capacity) {
            return false;
        }
        auto* command = new (storage + offset) Command(std::forward<Func>(func));
        if (last) {
            last->next = command;
        } else {
            first = command;
        }
        last = command;
        used = offset + sizeof(Command);
        PrefetchAhead();
        return true;
    }

    /// Raw storage in the chunk for a command's variable-length data; null when full.
    void* Allocate(size_t bytes, size_t align) {
        const size_t offset = (used + align - 1) & ~(align - 1);
        if (offset + bytes > Capacity) {
            return nullptr;
        }
        used = offset + bytes;
        PrefetchAhead();
        return storage + offset;
    }

    void Execute(vk::CommandBuffer cmdbuf) {
        for (CommandBase* command = first; command;) {
            CommandBase* const next = command->next;
            command->Execute(cmdbuf);
            command->~CommandBase();
            command = next;
        }
        first = last = nullptr;
        used = 0;
    }

    [[nodiscard]] bool Empty() const noexcept {
        return first == nullptr;
    }

    [[nodiscard]] size_t Size() const noexcept {
        return used;
    }

private:
    /// The recording thread last read these cache lines: the first write to each has to take
    /// it back from that core. Requesting ownership a few lines ahead overlaps those
    /// transfers instead of stalling on each (RecordPrefetch).
    void PrefetchAhead() const {
        if (!BbToggle::Disabled(BbToggle::RecordPrefetch)) {
            __builtin_prefetch(storage + used + 384, 1, 3);
            __builtin_prefetch(storage + used + 448, 1, 3);
        }
    }

    struct CommandBase {
        virtual ~CommandBase() = default;
        virtual void Execute(vk::CommandBuffer cmdbuf) = 0;
        CommandBase* next{};
    };
    template <typename Func>
    struct TypedCommand final : CommandBase {
        explicit TypedCommand(Func&& func_) : func{std::move(func_)} {}
        explicit TypedCommand(const Func& func_) : func{func_} {}
        void Execute(vk::CommandBuffer cmdbuf) override {
            func(cmdbuf);
        }
        Func func;
    };

    alignas(64) std::byte storage[Capacity];
    size_t used = 0;
    CommandBase* first{};
    CommandBase* last{};
};

class Scheduler {
public:
    /// `threaded_recording`: Vulkan commands passed to Record() are recorded by a worker thread.
    explicit Scheduler(const Instance& instance, bool threaded_recording = false);
    ~Scheduler();
#ifdef __APPLE__
    void InitPerf(bool present);
    void PerfMark(BbPerf::Phase phase, u64 frame = 0);
    class PerfScope {
        Scheduler& scheduler;
        BbPerf::Phase previous;
        bool active;
    public:
        PerfScope(Scheduler& scheduler, BbPerf::Phase phase)
            : scheduler{scheduler}, previous{scheduler.perf_phase}, active{bool(scheduler.perf)} {
            if (active) scheduler.PerfMark(phase);
        }
        ~PerfScope() { if (active) scheduler.PerfMark(previous); }
    };
#endif

    /// Sends the current execution context to the GPU
    /// and increments the scheduler timeline semaphore.
    /// Returns the timeline value the submission signals.
    u64 Flush(SubmitInfo& info);

    /// The next submission waits for `semaphore` to reach `value` before any of its commands
    /// (MetalFX: its output is consumed after the Metal queue signals; docs/macos/
    /// metalfx-integration.md). Waits added this way are consumed by that one submission.
    void WaitBeforeNextSubmit(vk::Semaphore semaphore, u64 value);

    /// Sends the current execution context to the GPU
    /// and increments the scheduler timeline semaphore.
    u64 Flush();

    /// Sends the current execution context to the GPU and waits for it to complete.
    void Finish();

#ifdef __APPLE__
    /// Teardown (docs/macos/moltenvk-prefill.md): submits what is recorded through the normal
    /// submission (its submit callback may still record its final commands, e.g. pending
    /// barriers), opens no further command buffer, then seals recording and waits for that
    /// submission's tick on the GPU. After Close no command buffer is open, so none is reset or
    /// destroyed unsubmitted at teardown (MoltenVK commits such a buffer when its prefill is on,
    /// outside our lifetime tracking; prefill is shelved but the teardown no longer depends on it). Only the GPU work is waited for: priority operations and
    /// deferred signals (labels at completion) run on their own threads and are not drained.
    /// Recording after Close stops; Flush and Finish return at once (owners destroyed later,
    /// e.g. the upscaler, finish their work this way). ~Scheduler closes an unclosed scheduler
    /// whose open buffer holds no recorded command, and stops if it holds one.
    void Close();
#endif

    /// bbport: waits for the GPU to retire the work submitted so far but leaves the command
    /// buffer being recorded open, so a CommandBuffer() the caller holds stays valid (Finish()
    /// submits it: recording into a submitted buffer crashes AMD's driver).
    void WaitPreviousRetired();

#ifdef __APPLE__
    /// Asynchronous submission (docs/macos/async-submission.md): waits until the submission with
    /// timeline value `tick` has been handed to vkQueueSubmit (not that it completed). Stops for
    /// a value not yet assigned (>= CurrentTick()) or never enqueued, and when submission failed
    /// or the scheduler closed before it. Call outside submit_mutex.
    void WaitQueueSubmitted(u64 tick);
    /// A submission's semaphore waits on an asynchronously submitting scheduler's timeline:
    /// each such value is submitted first (a waiter ahead of its producer in the one in-order
    /// Metal queue would block it). Stops for a wait on this scheduler's own unsubmitted value.
    void WaitDependencies(const SubmitInfo& info, u64 own_value);
    /// The tick of the command buffer being recorded where this runs: in a Record() closure on
    /// the recording thread, the submission it lands in (CurrentTick() there is the owner's open
    /// value, possibly several submissions ahead).
    [[nodiscard]] u64 RecordingTick() const noexcept {
        return recording_tick.load(std::memory_order_acquire);
    }
    /// Command buffers the pool holds (bounded: CommandPool::MaxBuffers), and the commits that
    /// waited for the GPU at that bound; readable from any thread.
    [[nodiscard]] size_t CommandBuffersAllocated() const noexcept {
        return command_pool.Size();
    }
    [[nodiscard]] u64 CommandBufferWaitsAtCap() const noexcept {
        return command_pool.WaitsAtCap();
    }
#endif

    /// Waits for the given tick to trigger on the GPU.
    void Wait(u64 tick);

    /// Attempts to execute operations whose tick the GPU has caught up with.
    void PopPendingOperations();

    /// Starts a new rendering scope with provided state.
    void BeginRendering(const RenderState& new_state);

    /// Ends current rendering scope.
    [[gnu::noinline]] void EndRendering();
#ifdef __APPLE__
    /// EndRendering, counted by its cause (bbport_pass_census.h) rather than by caller.
    void EndRenderingFor(BbPassCensus::Why why) {
        end_reason = why;
        EndRendering();
    }
#endif

    /// Sets a function to be called on every scheduler submission.
    void SetSubmitCallback(SubmitFunc&& on_submit) {
        this->on_submit = std::move(on_submit);
    }

    /// Returns the current render state.
    const RenderState& GetRenderState() const {
        return render_state;
    }

    /// Returns the current pipeline dynamic state tracking.
    DynamicState& GetDynamicState() {
        return dynamic_state;
    }

    /// Returns the current command buffer for recording on the calling thread. With threaded
    /// recording this first waits until all commands passed to Record() are recorded, then
    /// records directly (Record() included, so callers may mix both) until the next
    /// KickRecording() or submission.
    [[gnu::noinline]] vk::CommandBuffer CommandBuffer() {
#ifdef __APPLE__
        if (closed) [[unlikely]] {
            StopRecordingAfterClose();
        }
        recorded_since_begin = true;
#endif
        if (recording_threaded && !direct_mode) {
            SyncRecording();
            direct_mode = true;
            direct_recordings.fetch_add(1, std::memory_order_relaxed);
            TraceDirectRecording(__builtin_return_address(0));
        }
        return current_cmdbuf;
    }

    /// BB_RECORDER_TRACE=1: prints the most frequent CommandBuffer() callers every 2000 calls.
    static void TraceDirectRecording(void* caller);

    /// Records `func(vk::CommandBuffer)` in order with other commands. The closure must own
    /// everything it uses (capture by value): it may run later on the recording thread.
    template <typename Func>
    void Record(Func&& func) {
#ifdef __APPLE__
        if (closed) [[unlikely]] {
            StopRecordingAfterClose();
        }
        recorded_since_begin = true;
#endif
        if (!recording_threaded || direct_mode) {
            func(current_cmdbuf);
            return;
        }
        if (BbToggle::Disabled(BbToggle::ThreadedRecording)) {
            SyncRecording();
            direct_mode = true;
            func(current_cmdbuf);
            return;
        }
        if (!record_chunk->Push(std::forward<Func>(func))) {
            full_chunks.push_back(std::move(record_chunk));
            record_chunk = AcquireChunk();
            const bool pushed = record_chunk->Push(std::forward<Func>(func));
            ASSERT(pushed);
        }
    }

    /// True when Record() defers commands (and RecordData() copies into chunks).
    [[nodiscard]] bool IsRecordingDeferred() const noexcept {
        return recording_threaded && !direct_mode &&
               !BbToggle::Disabled(BbToggle::ThreadedRecording);
    }

    /// Makes room for `bytes` of RecordData() plus the command that uses them in the current
    /// chunk: data and command must share a chunk, which is recycled once executed.
    void ReserveRecordData(size_t bytes) {
        if (!IsRecordingDeferred()) {
            return;
        }
        ASSERT(bytes + 1024 <= RecordChunk::Capacity);
        if (RecordChunk::Capacity - record_chunk->Size() < bytes + 1024) {
            full_chunks.push_back(std::move(record_chunk));
            record_chunk = AcquireChunk();
        }
    }

    /// Copies `data` into recording storage that lives until the command that uses it has
    /// been recorded (the same chunk). With threaded recording off, returns `data` itself.
    template <typename T>
    std::span<const T> RecordData(std::span<const T> data) {
        if (!IsRecordingDeferred() || data.empty()) {
            return data;
        }
        // Room for the data and the command that follows it, so both stay in one chunk.
        const size_t bytes = data.size_bytes();
        ReserveRecordData(bytes + alignof(T));
        auto* dst = static_cast<T*>(record_chunk->Allocate(bytes, alignof(T)));
        std::memcpy(dst, data.data(), bytes);
        return {dst, data.size()};
    }

    /// Hands recorded commands to the recording thread. Called at points where no caller holds
    /// the raw command buffer (end of draws and dispatches); small batches are kept.
    void KickRecording(bool force = false);

    /// Waits until every recorded command is in the command buffer.
    void SyncRecording();

    /// bbport: guest memory copies on the recording thread (small ones, RecordHostCopy) or the
    /// copy threads (BbCopy::Async) must be done before the guest learns the GPU is past them
    /// (it may then rewrite the memory, e.g. UI vertices: flickering) and before the
    /// submission that reads them. Waits for all.
    void WaitHostCopies();

    /// Runs `copy` on the recording thread in order with the commands (the thread spins for
    /// work anyway, so small copies cost no wakeup); WaitHostCopies() covers it.
    template <typename Func>
    void RecordHostCopy(Func&& copy) {
        Record([copy = std::forward<Func>(copy), this,
                seq = ++host_copies_issued](vk::CommandBuffer) {
            copy();
            host_copies_done.store(seq, std::memory_order_release);
        });
    }

    /// bbport: runs `signal` once the guest memory copies issued so far are done, without
    /// waiting here: the recording thread reaches it after the copies queued before it and hands
    /// it to the copy threads' completion (BbCopy::AfterCopies).
    void SignalAfterHostCopies(std::function<void()> signal);

#ifdef __APPLE__
    /// bbport (Apple, labels at GPU completion, the default): like SignalAfterHostCopies, but
    /// `signal` runs only once the GPU has completed the command buffer holding everything
    /// recorded before it. That buffer is submitted at once, so the GPU reaches it without
    /// waiting for the frame's end (a guest or WAIT_REG_MEM waiting on the label would never
    /// see it otherwise). Signals keep their order, and WaitDeferredSignals waits for them.
    /// `batch` (stage B): the command buffer is not submitted at once; FlushLabels submits it
    /// at the next packet boundary where the draw pipe goes idle or the oldest label waited
    /// LabelBatchAge.
    void SignalAtGpuCompletion(std::function<void()> signal, bool batch = false);

    /// Stage B, between packets: submits the labels recorded since the last submission when
    /// `idle` or once the oldest has waited LabelBatchAge.
    void FlushLabels(bool idle);

    /// Waits in slices until the GPU passed timeline value `tick` of this scheduler; false if
    /// `stoken` was stopped first. A failed wait (device loss) stops loudly.
    bool WaitRetired(u64 tick, const std::stop_token& stoken) {
        return WaitLabelTick(tick, stoken);
    }
    static constexpr auto LabelBatchAge = std::chrono::microseconds(500);

    /// Whether labels are written at GPU completion now: the default, unless toggle bit 59
    /// (LabelsAtSubmit, live) or BB_LABELS_AT_GPU_COMPLETE=0/1 for the whole run says otherwise
    /// (bbport_label_mode.h). Read when a label is issued.
    static bool LabelsAtGpuComplete();

    /// GpuIdle's wait split (docs/macos/throttle-relaxation.md, R0): waits only for the
    /// recorder-side deferred writes (host copies, RecorderFences), not the completion labels.
    /// Called by the thread recording (stage A), after the draw pipe drained.
    void WaitRecorderSignals();
    /// Completion labels issued so far, and the timeline tick the last one waits for.
    [[nodiscard]] u64 LabelsIssued() const noexcept {
        return completion_labels.Issued();
    }
    [[nodiscard]] u64 LastLabelTick() const noexcept {
        return last_label_tick.load(std::memory_order_relaxed);
    }
#endif

    /// bbport: before a guest-visible write that is not deferred (WriteData, end-of-shader
    /// fences, flip): waits until every signal handed to SignalAfterHostCopies ran, so that
    /// the write cannot overtake an earlier fence. Otherwise the guest, seeing the later value,
    /// may free memory the earlier fence then writes into (corrupted heap, guest fault).
    void WaitDeferredSignals();

    /// Whether a render pass with exactly this state is open.
    [[nodiscard]] bool IsRendering() const {
        return is_rendering;
    }

    [[nodiscard]] bool IsRenderingWith(const RenderState& state) const {
        return is_rendering && render_state == state;
    }

    /// Whether BeginRendering(state) would start a new render pass.
    [[nodiscard]] bool WillBeginRendering(const RenderState& state) const {
        return !(is_rendering && render_state == state);
    }

    /// CommandBuffer() calls that waited for a recording thread (BB_FRAME_STATS).
    static inline std::atomic<u64> direct_recordings{0};

    /// Returns the current command buffer tick.
    [[nodiscard]] u64 CurrentTick() const noexcept {
        return work_semaphore.CurrentTick();
    }

    /// Returns true when a tick has been triggered by the GPU.
    [[nodiscard]] bool IsFree(u64 tick) noexcept {
        if (work_semaphore.IsFree(tick)) {
            return true;
        }
        work_semaphore.Refresh();
        return work_semaphore.IsFree(tick);
    }

    /// Returns the scheduler timeline semaphore.
    [[nodiscard]] Semaphore* GetWorkSemaphore() noexcept {
        return &work_semaphore;
    }

#ifdef __APPLE__
    /// bbport (frame capture): deferred operations not yet run, and labels issued and not yet
    /// written (BB_LABELS_AT_GPU_COMPLETE).
    [[nodiscard]] u32 NumPendingOperations() const noexcept {
        return num_pending_ops.load(std::memory_order_acquire);
    }
    /// Priority operations issued and not yet run to the end (including the one the runner
    /// thread has taken and is waiting for or running).
    [[nodiscard]] u64 PriorityOperationsOutstanding() const noexcept {
        return priority_ops_issued.load(std::memory_order_acquire) -
               priority_ops_completed.load(std::memory_order_acquire);
    }
    /// Deferred signals (labels, fences) issued and not yet written, whether or not their
    /// writer has taken them.
    [[nodiscard]] u64 DeferredSignalsOutstanding() const noexcept {
        return deferred_signals_issued.load(std::memory_order_acquire) -
               deferred_signals_done->load(std::memory_order_acquire);
    }
    /// The frame capture's completion check (Rasterizer::QuiesceForCapture calls it after
    /// Finish): true once no deferred operation, priority operation or deferred signal issued
    /// so far is left; polled without blocking (1 ms sleeps) until a `limit` deadline (sleep
    /// scheduling can overshoot it slightly), then false and `why` with the counts. It does not
    /// run or help the work it waits for.
    bool WaitQuiescentForCapture(std::chrono::milliseconds limit, std::string& why);
#endif

    /// Defers an operation until the gpu has reached the current cpu tick.
    /// Will be run when submitting or calling PopPendingOperations.
    void DeferOperation(Common::UniqueFunction<void>&& func) {
        std::unique_lock lk(pending_ops_mutex);
        pending_ops.emplace(std::move(func), CurrentTick());
        num_pending_ops.fetch_add(1, std::memory_order_release);
    }

    /// Defers an operation until the gpu has reached the current cpu tick.
    /// Runs as soon as possible in another thread.
    void DeferPriorityOperation(Common::UniqueFunction<void>&& func) {
#ifdef __APPLE__
        priority_ops_issued.fetch_add(1, std::memory_order_acq_rel);
#endif
        {
            std::unique_lock lk(priority_pending_ops_mutex);
            priority_pending_ops.emplace(std::move(func), CurrentTick());
        }
        priority_pending_ops_cv.notify_one();
    }

    static std::mutex submit_mutex;

private:
    void AllocateWorkerCommandBuffers();

    std::unique_ptr<RecordChunk> AcquireChunk();

    void RecorderThread(std::stop_token stoken);

    u64 SubmitExecution(SubmitInfo& info, u64 sealed_value = 0);

#ifdef __APPLE__
    /// What the recording thread needs to end, submit and begin: owned copies, never the
    /// owner's later state.
    struct SubmitPacket {
        SubmitInfo info;
        u64 signal_value = 0;
        u64 next_tick = 0;      ///< the next submission's value: the begun buffer's pool stamp
        u64 copies_through = 0; ///< host copies this submission reads (a sealed prefix)
        BbPerf::Phase perf_phase = BbPerf::Phase::Scene;
        u64 perf_frame = 0;
    };
    /// The owner's half of Flush with asynchronous submission: assigns the value, records the
    /// submission's last commands, enqueues the packet and returns.
    u64 EnqueueSubmission(SubmitInfo& info, u64 signal_value);
    /// Assigns the value and completes `info`: the labels check, on_submit (it records into this
    /// submission), WaitBeforeNextSubmit's waits.
    u64 SealSubmission(SubmitInfo& info);
    /// Stops when called on this scheduler's recording thread (it would wait for itself).
    void StopOnRecorderThread(const char* what) const;
    /// The recording thread's half: end, wait for the sealed copies, vkQueueSubmit, begin.
    void SubmitFromRecorder(SubmitPacket& packet);
    /// Owner: chunks it holds go to the recorder, without touching record_chunk.
    void QueueFullChunks();
    /// The asynchronously submitting schedulers' timelines (WaitDependencies).
    static void RegisterAsyncTimeline(vk::Semaphore semaphore, Scheduler* owner);
    static void UnregisterAsyncTimeline(Scheduler* owner);
    static Scheduler* AsyncTimelineOwner(vk::Semaphore semaphore);
#endif

    void PriorityPendingOpsThread(std::stop_token stoken);

private:
    const Instance& instance;
    Semaphore work_semaphore;
    CommandPool command_pool;
    DynamicState dynamic_state;
    SubmitFunc on_submit{};
    vk::CommandBuffer current_cmdbuf;
#ifdef __APPLE__
    std::unique_ptr<BbPerf::GpuStream> perf;
    BbPerf::Phase perf_phase = BbPerf::Phase::Scene;
    u64 perf_frame = 0;
#endif
    std::condition_variable_any event_cv;
    struct PendingOp {
        Common::UniqueFunction<void> callback;
        u64 gpu_tick;
    };
    std::queue<PendingOp> pending_ops;
    std::recursive_mutex pending_ops_mutex;
    u32 pending_polls = 0; // bbport
    std::atomic<u32> num_pending_ops{0}; ///< bbport: pending_ops.size(), checked without the lock
    std::queue<PendingOp> priority_pending_ops;
    std::mutex priority_pending_ops_mutex;
    std::condition_variable_any priority_pending_ops_cv;
    std::jthread priority_pending_ops_thread;
#ifdef __APPLE__
    std::atomic<u64> priority_ops_issued{0}, priority_ops_completed{0}; ///< frame capture
#endif
    RenderState render_state;
    bool is_rendering = false;
    // bbport: threaded recording
    std::unique_ptr<RecordChunk> record_chunk;
    std::vector<std::unique_ptr<RecordChunk>> full_chunks;
    std::mutex recorder_mutex;
    std::condition_variable_any recorder_cv;
    std::condition_variable_any recorder_idle_cv;
#ifdef __APPLE__
    /// A recorder queue entry: a chunk of commands, or (asynchronous submission) a submission.
    struct RecorderItem {
        std::unique_ptr<RecordChunk> chunk;
        std::unique_ptr<SubmitPacket> submit;
    };
    std::deque<RecorderItem> recorder_queue;
    bool async_submit = false;      ///< Apple, threaded recording: Flush enqueues (fixed)
    u32 queued_submits = 0;         ///< enqueued, not yet submitted (recorder_mutex)
    static constexpr u32 MaxQueuedSubmits = 8;
    size_t live_chunks = 0;         ///< chunks allocated (recorder_mutex)
    static constexpr size_t MaxChunks = 128; ///< 16 MiB of recorded commands in flight
    std::condition_variable_any recorder_space_cv; ///< a submit or a chunk was freed
    std::atomic<u64> enqueued_through{0};  ///< highest value enqueued
    std::atomic<u64> submitted_through{0}; ///< highest value handed to vkQueueSubmit
    bool submit_failed = false;     ///< recorder_mutex
    /// The scheduler whose recording thread this is (set by that thread, read by itself only).
    static inline thread_local const Scheduler* recording_for = nullptr;
    std::atomic<u64> recording_tick{0}; ///< the tick the buffer being recorded is stamped with
#else
    std::deque<std::unique_ptr<RecordChunk>> recorder_queue;
#endif
    std::vector<std::unique_ptr<RecordChunk>> free_chunks;
    bool recorder_busy = false;
    std::atomic<size_t> queued_chunks{0}; ///< recorder_queue.size() for lock-free polling
    bool recorder_sleeping = false; ///< waiting on recorder_cv (guarded by recorder_mutex)
    bool direct_mode = false; ///< the command buffer is recorded on the caller's thread
#ifdef __APPLE__
    BbPassCensus::Why end_reason = BbPassCensus::Why::Other; ///< for the next EndRendering
    [[noreturn, gnu::cold, gnu::noinline]] static void StopRecordingAfterClose();
    bool closing = false; ///< Close() submits the last command buffer: open no new one
    bool closed = false;  ///< Close() submitted it: recording is sealed
    bool recorded_since_begin = false; ///< the open command buffer has a recorded command
#endif
    u64 host_copies_issued = 0;
    std::atomic<u64> host_copies_done{0};
    /// WaitBeforeNextSubmit, consumed by SubmitExecution (both under submit_mutex).
    std::vector<std::pair<vk::Semaphore, u64>> next_submit_waits;
    std::atomic<u64> deferred_signals_issued{0}; ///< by the thread recording (A or B)
    std::shared_ptr<std::atomic<u64>> deferred_signals_done =
        std::make_shared<std::atomic<u64>>(0);
    // bbport: whether recorder_thread runs, read on every Record(). std::jthread::joinable()
    // compares thread ids, which on Windows (libc++ over winpthreads) costs two system calls.
    bool recording_threaded{false};
#ifdef __APPLE__
public:
    /// The draw scheduler (Rasterizer's): its submissions are counted by reason (parse split).
    void EnableFlushCensus() {
        flush_census = true;
    }

private:
    bool flush_census = false;
#endif
    std::jthread recorder_thread;
#ifdef __APPLE__
    /// SignalAtGpuCompletion's writer: labels only, in issue order (BbLabels::Sequencer), each
    /// after its own (already submitted) timeline tick. Not the priority runner, whose other ops
    /// may wait for ticks submitted only at the frame's end. Declared last: the thread stops
    /// before the sequencer goes.
    void LabelThread(std::stop_token stoken);
    /// Waits in slices until the GPU passed `tick`; false if stopped first.
    bool WaitLabelTick(u64 tick, const std::stop_token& stoken);
    BbLabels::Sequencer labels;
    /// Labels recorded and not yet submitted (batched), the tick their submission must signal,
    /// and when the oldest was recorded. Set by the recording thread, cleared by a submission.
    std::atomic<bool> labels_unflushed{false};
    u64 unflushed_tick = 0;
    /// Of deferred_signals_issued/done, by kind (bbport_deferred_counts.h): the recorder-side
    /// writes and the completion labels, each counted on its own.
    BbDeferred::Count recorder_signals;
    BbDeferred::Count completion_labels;
    std::atomic<u64> last_label_tick{0};
    std::chrono::steady_clock::time_point unflushed_since{};
    std::once_flag label_thread_once;
    std::jthread label_thread;
#endif
    tracy::VkCtxScope* profiler_scope{};
};

} // namespace Vulkan
