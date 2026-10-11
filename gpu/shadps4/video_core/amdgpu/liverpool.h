// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdlib>

#include <condition_variable>
#include <deque>
#include <memory>
#include <coroutine>
#include <exception>
#include <mutex>
#include <semaphore>
#include <span>
#include <thread>
#include <vector>
#include <queue>

#include "common/assert.h"
#ifdef __APPLE__
#include "bbport_cmd_data.h"
#endif
#include "common/slot_vector.h"
#include "common/types.h"
#include "common/unique_function.h"
#include "video_core/amdgpu/cb_db_extent.h"
#include "video_core/amdgpu/cmd_snapshot.h"
#include "video_core/amdgpu/regs.h"

namespace Vulkan {
class Rasterizer;
}

namespace Libraries::VideoOut {
struct VideoOutPort;
}

namespace AmdGpu {

union PM4Header;

struct Liverpool {
    static constexpr u32 GfxQueueId = 0u;
    static constexpr u32 NumGfxRings = 1u;     // actually 2, but HP is reserved by system software
    static constexpr u32 NumComputePipes = 7u; // actually 8, but #7 is reserved by system software
    static constexpr u32 NumQueuesPerPipe = 8u;
    static constexpr u32 NumComputeRings = NumComputePipes * NumQueuesPerPipe;
    static constexpr u32 NumTotalQueues = NumGfxRings + NumComputeRings;
    static_assert(NumTotalQueues < 64u); // need to fit into u64 bitmap for ffs

    enum ContextRegs : u32 {
        DbZInfo = 0xA010,
        CbColor0Base = 0xA318,
        CbColor1Base = 0xA327,
        CbColor2Base = 0xA336,
        CbColor3Base = 0xA345,
        CbColor4Base = 0xA354,
        CbColor5Base = 0xA363,
        CbColor6Base = 0xA372,
        CbColor7Base = 0xA381,
        CbColor0Cmask = 0xA31F,
        CbColor1Cmask = 0xA32E,
        CbColor2Cmask = 0xA33D,
        CbColor3Cmask = 0xA34C,
        CbColor4Cmask = 0xA35B,
        CbColor5Cmask = 0xA36A,
        CbColor6Cmask = 0xA379,
        CbColor7Cmask = 0xA388,
    };

    Regs regs{};
    std::array<CbDbExtent, NUM_COLOR_BUFFERS> last_cb_extent{};
    CbDbExtent last_db_extent{};
    /// bbport: register blocks written since the last draw handed to the draw recording thread
    /// (Rasterizer::PostDraw sends them along).
    RegDirty pipe_dirty;
    /// bbport: fence writes handed to the draw recording thread and not yet done there: a
    /// WaitRegMem on one of them is met in stream order without waiting for that thread.
    struct PendingFence {
        VAddr address;
        u64 data;
        u32 num_bytes;
        u64 position;
    };
    std::deque<PendingFence> pending_fences;
    void NotePendingFences(const auto& event);
    bool PendingFenceValue(VAddr address, u32& value);
    void NotePendingWrite(const struct PM4CmdWriteData& write_data, u32 num_bytes);
    /// bbport: running checksum of graphics-register packets (see ApplyGraphicsRegisterPacket).
    u64 gfx_reg_checksum{};
    /// Top-level graphics submissions, numbered for the draw preparation workers.
    static constexpr u64 NoSeq = ~0ull;
    u64 gfx_submit_seq{};
    /// bbport (macOS): GpuIdle is raised only while no submission is queued
    /// (docs/macos/gpu-idle.md). Statistics per 600 frames: graphics submissions queued ahead of
    /// the parser when the guest submits, and GpuIdle interrupts held back because the guest
    /// submitted while the previous ones were being retired.
    struct ParseLagStats {
        u64 frames{}, samples{}, queued_sum{}, queued_max{}, idle_held_back{};
        u64 snapshot_bytes{}, snapshot_ns{}, snapshot_ns_max{}, snapshot_writes_waited{};
        u64 prep_build_ns{}, prep_build_ns_max{}; ///< draw preparation copy, on the guest thread
    } parse_lag;
    /// GpuIdle's wait split (docs/macos/throttle-relaxation.md, R0), per frame end that raised
    /// GpuIdle: from the guest's sceGnmSubmitDone to GpuIdle, and its parts (GPU thread only).
    struct IdleSplitStats {
        u64 frames{}, total_ns{}, total_ns_max{}, drain_ns{}, recorder_ns{}, labels_ns{};
        u64 tail_frames{}, tail_ticks{}, unlabeled_frames{}, unpaired{};
        u64 labels_issued_before{};
    } idle_split;
    std::chrono::steady_clock::time_point submit_done_at{}; ///< under submit_mutex
    std::chrono::steady_clock::time_point idle_split_frame_submit_done{}; ///< GPU thread
    void NoteIdleSplit(u64 drain_ns, u64 recorder_ns, u64 labels_ns);
    /// bbport (macOS): graphics submissions whose command memory is a submit-time copy and not
    /// fully parsed yet (cmd_snapshot.h), in submission order.
    std::mutex snapshots_mutex;
    std::deque<std::shared_ptr<CmdSnapshot>> gfx_snapshots;
    u64 snapshot_submits{};
    /// GPU command thread: a replay capture holds the next frame's submissions (replay_capture.h).
    bool replay_hold{};
    u32 replay_hold_commands{};
    /// After a hold ended on a CPU flip: per queue, the held tasks still to complete. Until they
    /// have, ProcessCommands leaves a CPU flip (and what is queued behind it) waiting, so the flip
    /// does not present or publish the held frame before its draws parse.
    std::array<u32, NumTotalQueues> replay_held_tasks{};
    u32 replay_held_left{};
#ifdef __APPLE__
    /// BB_CMD_DATA_CHECK (bbport_cmd_data.h): where `size` bytes at `address`, read as
    /// `bytes_read`, lie relative to the submitted command memory snapshots still held.
    static BbCmdData::Result LookupCommandData(u64 address, u64 size, const void* bytes_read);
#endif
    /// A stream write the PS4 command processor would see in command memory a copy hides.
    void CheckCommandMemoryWrite(VAddr address, u64 size, const char* writer);
    static u64 HashRegisterPacket(u64 checksum, const u32* words, u32 count);
    /// `dirty` (draw-preparation scanner) records the register blocks written.
    static void ApplyGraphicsRegisterPacket(Regs& regs, const PM4Header* header, u64& checksum,
                                            RegDirty* dirty = nullptr);

public:
    explicit Liverpool();
    ~Liverpool();

    void SubmitGfx(std::span<const u32> dcb, std::span<const u32> ccb);
    void SubmitAsc(u32 gnm_vqid, std::span<const u32> acb);

    void SubmitDone() noexcept {
        std::scoped_lock lk{submit_mutex};
#ifdef __APPLE__
        submit_done_at = std::chrono::steady_clock::now(); // GpuIdle's wait split (R0)
#endif
        mapped_queues[GfxQueueId].ccb_buffer_offset = 0;
        mapped_queues[GfxQueueId].dcb_buffer_offset = 0;
        submit_done = true;
        submit_cv.notify_one();
    }

    void WaitGpuIdle() noexcept {
        std::unique_lock lk{submit_mutex};
        submit_cv.wait(lk, [this] { return num_submits == 0; });
    }

    /// bbport: also the draw recording thread and the fences it deferred are done (see
    /// work_retired). sceGnmSubmitDone does not block the guest when this is true.
    bool IsGpuIdle() const {
        static const bool use_retired = [] {
            const char* env = std::getenv("BB_WORK_RETIRED");
            return !(env && env[0] == '0');
        }();
        return num_submits == 0 && (work_retired || !use_retired);
    }

    void SetVoPort(Libraries::VideoOut::VideoOutPort* port) {
        vo_port = port;
    }

    void BindRasterizer(Vulkan::Rasterizer* rasterizer_) {
        rasterizer = rasterizer_;
    }

    /// Runs `func` on the GPU command thread. `cpu_flip` marks a CPU (non-EOP) flip, which a
    /// replay capture hold must not run ahead of the held frame.
    template <bool wait_done = false>
    void SendCommand(auto&& func, bool cpu_flip = false) {
        if constexpr (wait_done) {
            std::binary_semaphore sem{0};
            {
                std::scoped_lock lk{submit_mutex};
                command_queue.push({[&sem, &func] {
                                        func();
                                        sem.release();
                                    },
                                    cpu_flip});
                ++num_commands;
                submit_cv.notify_one();
            }
            sem.acquire();
        } else {
            std::scoped_lock lk{submit_mutex};
            command_queue.push({std::move(func), cpu_flip});
            ++num_commands;
            submit_cv.notify_one();
        }
    }

    void ReserveCopyBufferSpace() {
        GpuQueue& gfx_queue = mapped_queues[GfxQueueId];
        std::scoped_lock lk(gfx_queue.m_access);
        constexpr size_t GfxReservedSize = 2_MB >> 2;
        gfx_queue.ccb_buffer.reserve(GfxReservedSize);
        gfx_queue.dcb_buffer.reserve(GfxReservedSize);
    }

    inline ComputeProgram& GetCsRegs() {
        return mapped_queues[curr_qid].cs_state;
    }

    struct AscQueueInfo {
        static constexpr size_t Pm4BufferSize = 1024;
        VAddr map_addr;
        u32* read_addr;
        u32 ring_size_dw;
        u32 pipe_id;
        std::array<u32, Pm4BufferSize> tmp_packet;
        u32 tmp_dwords;
    };
    Common::SlotVector<AscQueueInfo> asc_queues{};

    std::thread::id GetGpuCommandProcessorThread() {
        return gpu_id;
    }

#if defined(__linux__) || defined(_WIN32) || defined(__APPLE__)
    u32 GetGpuCommandProcessorThreadId() {
        return gpu_tid;
    }
#endif

private:
    struct Task {
        struct promise_type {
            auto get_return_object() {
                Task task{};
                task.handle = std::coroutine_handle<promise_type>::from_promise(*this);
                return task;
            }
            static constexpr std::suspend_always initial_suspend() noexcept {
                // We want the task to be suspended at start
                return {};
            }
            static constexpr std::suspend_always final_suspend() noexcept {
                return {};
            }
            void unhandled_exception() {
                try {
                    std::rethrow_exception(std::current_exception());
                } catch (const std::exception& e) {
                    UNREACHABLE_MSG("Unhandled exception: {}", e.what());
                }
            }
            void return_void() {}
            struct empty {};
            std::suspend_always yield_value(empty&&) {
                return {};
            }
        };

        using Handle = std::coroutine_handle<promise_type>;
        Handle handle;
    };

    using CmdBuffer = std::pair<std::span<const u32>, std::span<const u32>>;
    CmdBuffer CopyCmdBuffers(std::span<const u32> dcb, std::span<const u32> ccb);
    /// `snapshot`/`region` (macOS): the buffer is a submit-time copy (cmd_snapshot.h).
    Task ProcessGraphics(std::span<const u32> dcb, std::span<const u32> ccb, u64 seq = NoSeq,
                         std::shared_ptr<CmdSnapshot> snapshot = {},
                         CmdSnapshot::Region* region = nullptr);
    Task ProcessCeUpdate(std::span<const u32> ccb, std::shared_ptr<CmdSnapshot> snapshot = {},
                         CmdSnapshot::Region* region = nullptr);
    template <bool is_indirect = false>
    Task ProcessCompute(std::span<const u32> acb, u32 vqid);

    /// Runs queued host commands; returns how many ran. With `stopped_at_cpu_flip` (replay
    /// capture hold) a CPU flip at the front stays queued and sets it; so it does while held
    /// work from a hold that ended on a CPU flip is still parsing (replay_held_left).
    u32 ProcessCommands(bool* stopped_at_cpu_flip = nullptr);
    void Process(std::stop_token stoken);

    struct GpuQueue {
        std::mutex m_access{};
        std::atomic<u32> dcb_buffer_offset;
        std::atomic<u32> ccb_buffer_offset;
        std::vector<u32> dcb_buffer;
        std::vector<u32> ccb_buffer;
        std::queue<Task::Handle> submits{};
        ComputeProgram cs_state{};
    };
    std::array<GpuQueue, NumTotalQueues> mapped_queues{};
    u32 num_mapped_queues{1u}; // GFX is always available

    VAddr indirect_args_addr{};
    u32 num_counter_pairs{};
    u64 pixel_counter{};

    struct ConstantEngine {
        void Reset() {
            ce_count = 0;
            de_count = 0;
            ce_compare_count = 0;
        }

        [[nodiscard]] u32 Diff() const {
            ASSERT_MSG(ce_count >= de_count, "DE counter is ahead of CE");
            return ce_count - de_count;
        }

        u32 ce_compare_count{};
        u32 ce_count{};
        u32 de_count{};
        static std::array<u8, 48_KB> constants_heap;
    } cblock{};

    Vulkan::Rasterizer* rasterizer{};
    Libraries::VideoOut::VideoOutPort* vo_port{};
    const bool guest_markers_enabled;
    std::jthread process_thread{};
    std::atomic<u32> num_submits{};
    /// bbport: false from a submission until its draws and deferred fences are done (stage A
    /// decrements num_submits once it has decoded a submission, before that). Under
    /// submit_mutex with num_submits.
    std::atomic<bool> work_retired{true};
    std::atomic<u32> num_commands{};
    std::atomic<bool> submit_done{};
    std::mutex submit_mutex;
    std::condition_variable_any submit_cv;
    struct HostCommand {
        Common::UniqueFunction<void> func;
        bool cpu_flip;
    };
    std::queue<HostCommand> command_queue{};
    std::thread::id gpu_id;
#if defined(__linux__) || defined(_WIN32) || defined(__APPLE__)
    u32 gpu_tid{};
#endif
    s32 curr_qid{-1};
};

} // namespace AmdGpu
