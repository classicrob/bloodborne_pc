// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <thread>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#ifndef _WIN32
#include <dlfcn.h>
#endif
#include <functional>

#include "bbport_copy.h"
#include "bbport_label_mode.h"
#include "video_core/renderer_vulkan/vk_gpu_profiler.h"
#include "bbport_toggles.h"
#include "common/assert.h"
#include "common/debug.h"
#include "common/thread.h"
#include "imgui/renderer/texture_manager.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "bbport_parse_split.h"
#include "bbport_pass_trace.h"
#include "bbport_hot_thread_qos.h"
#include "bbport_threads.h"

namespace Vulkan {

std::mutex Scheduler::submit_mutex;

Scheduler::Scheduler(const Instance& instance, bool threaded_recording)
    : instance{instance}, work_semaphore{instance}, command_pool{instance, &work_semaphore} {
    // bbport: BB_VK_RECORD_THREAD=0 records on the calling thread.
    const char* env = std::getenv("BB_VK_RECORD_THREAD");
    if (threaded_recording && !(env && env[0] == '0')) {
#ifdef __APPLE__
        // Asynchronous submission (docs/macos/async-submission.md): the recording thread ends,
        // submits and begins command buffers; Flush only enqueues. Set before the first chunk,
        // which the budget counts.
        async_submit = true;
        RegisterAsyncTimeline(work_semaphore.Handle(), this);
#endif
        record_chunk = AcquireChunk();
        recorder_thread = std::jthread(std::bind_front(&Scheduler::RecorderThread, this));
        recording_threaded = true;
    }
#if TRACY_GPU_ENABLED
    profiler_scope = reinterpret_cast<tracy::VkCtxScope*>(std::malloc(sizeof(tracy::VkCtxScope)));
#endif
    AllocateWorkerCommandBuffers();
    priority_pending_ops_thread =
        std::jthread(std::bind_front(&Scheduler::PriorityPendingOpsThread, this));
}

Scheduler::~Scheduler() {
#ifdef __APPLE__
    if (async_submit) {
        UnregisterAsyncTimeline(this);
    }
    // The open command buffer must not be destroyed unsubmitted (Close). One with recorded
    // commands at this point would reference resources its owner may already have destroyed.
    if (!closed) {
        if (recorded_since_begin) {
            std::fputs("STOP: a scheduler with unsubmitted recorded commands is destroyed "
                       "without Scheduler::Close\n",
                       stderr);
            ASSERT_MSG(false, "Scheduler destroyed with unsubmitted recorded commands");
        }
        Close();
    }
#endif
    if (recorder_thread.joinable()) {
        SyncRecording();
        recorder_thread.request_stop();
        recorder_cv.notify_all();
        recorder_thread.join();
    }
#if TRACY_GPU_ENABLED
    std::free(profiler_scope);
#endif
}

void Scheduler::BeginRendering(const RenderState& new_state) {
    if (is_rendering && render_state == new_state) {
#ifdef __APPLE__
        if (BbPassTrace::Enabled()) {
            BbPassTrace::DiscardViews(); // the open pass continues: no pass takes them
        }
#endif
        return;
    }
    BB_SPLIT_SCOPE(KRenderPassSwitch);
#ifdef __APPLE__
    EndRenderingFor(BbPassCensus::Why::TargetChange);
    // A pass with exactly the state of the pass that ended just before it: a split the targets
    // did not require (render_state still holds the ended pass's state).
    const bool resumed = has_render_state && render_state == new_state;
    BbPassCensus::NoteBegin(resumed);
    has_render_state = true;
#else
    EndRendering();
#endif
    is_rendering = true;
    render_state = new_state;
#ifdef __APPLE__
    if (BbPassTrace::Enabled()) {
        BbPassTrace::NoteBegin(render_state, resumed);
    }
#endif

    std::array<vk::RenderingAttachmentInfo, 8> color_attachments;
    for (u32 i = 0; i < render_state.num_color_attachments; ++i) {
        const auto& cb = render_state.color_attachments[i];
        color_attachments[i] = vk::RenderingAttachmentInfo{
            .imageView = cb.image_view,
            .imageLayout = cb.image_layout,
            .loadOp = cb.is_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
            .storeOp = vk::AttachmentStoreOp::eStore,
            .clearValue = vk::ClearValue{.color = vk::ClearColorValue{.uint32 = cb.clear_value}},
        };
    }

    const auto& db = render_state.depth_stencil_attachment;
    const vk::RenderingAttachmentInfo depth_attachment = {
        .imageView = db.image_view,
        .imageLayout = db.image_layout,
        .loadOp = db.depth_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue =
            vk::ClearValue{.depthStencil = vk::ClearDepthStencilValue{.depth = std::bit_cast<float>(
                                                                          db.clear_value[0])}},
    };
    const vk::RenderingAttachmentInfo stencil_attachment = {
        .imageView = db.image_view,
        .imageLayout = db.image_layout,
        .loadOp = db.stencil_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue = vk::ClearValue{.depthStencil =
                                         vk::ClearDepthStencilValue{.stencil = db.clear_value[1]}},
    };

    const vk::RenderingInfo rendering_info = {
        .renderArea =
            {
                .offset = {0, 0},
                .extent = {render_state.width, render_state.height},
            },
        .layerCount = render_state.num_layers,
        .colorAttachmentCount = render_state.num_color_attachments,
        .pColorAttachments = color_attachments.data(),
        .pDepthAttachment = db.has_depth ? &depth_attachment : nullptr,
        .pStencilAttachment = db.has_stencil ? &stencil_attachment : nullptr,
    };

    if (!recording_threaded) {
        BB_SPLIT_SCOPE(KEncRendering);
        current_cmdbuf.beginRendering(rendering_info);
        return;
    }
    // The attachment infos live on this stack frame: the recorded closure keeps copies.
    Record([info = rendering_info, color_attachments, depth_attachment,
            stencil_attachment](vk::CommandBuffer cmdbuf) mutable {
        BB_SPLIT_SCOPE(KEncRendering);
        info.pColorAttachments = color_attachments.data();
        if (info.pDepthAttachment) {
            info.pDepthAttachment = &depth_attachment;
        }
        if (info.pStencilAttachment) {
            info.pStencilAttachment = &stencil_attachment;
        }
        cmdbuf.beginRendering(info);
    });
}

void Scheduler::EndRendering() {
#ifdef __APPLE__
    const auto why = std::exchange(end_reason, BbPassCensus::Why::Other);
#endif
    if (!is_rendering) {
        return;
    }
#ifdef __APPLE__
    const void* ender = BbPassCensus::NoteEnd(why, __builtin_return_address(0));
    if (BbPassTrace::Enabled()) {
        BbPassTrace::NoteEnd(why, ender);
    }
#endif
    is_rendering = false;
    BB_SPLIT_SCOPE(KRenderPassSwitch);
    Record([](vk::CommandBuffer cmdbuf) {
        BB_SPLIT_SCOPE(KEncRendering);
        cmdbuf.endRendering();
    });
}

void Scheduler::TraceDirectRecording(void* caller) {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_RECORDER_TRACE");
        return env && env[0] == '1';
    }();
    if (!enabled) {
        return;
    }
    static std::mutex mutex;
    static std::unordered_map<void*, u64> callers;
    static u64 calls;
    std::scoped_lock lk{mutex};
    ++callers[caller];
    if (++calls % 2000) {
        return;
    }
    std::vector<std::pair<u64, void*>> top;
    for (const auto& [address, count] : callers) {
        top.emplace_back(count, address);
    }
    std::ranges::sort(top, std::greater{});
    for (size_t i = 0; i < std::min<size_t>(top.size(), 8); ++i) {
#ifdef _WIN32
        struct { const char* dli_fname; void* dli_fbase; } info{nullptr, nullptr};
#else
        Dl_info info{};
        dladdr(top[i].second, &info);
#endif
        std::printf("Recorder sync caller: %llu x %s+0x%lx\n",
                    static_cast<unsigned long long>(top[i].first),
                    info.dli_fname ? info.dli_fname : "?",
                    static_cast<unsigned long>(reinterpret_cast<uintptr_t>(top[i].second) -
                                               reinterpret_cast<uintptr_t>(info.dli_fbase)));
    }
    callers.clear();
}

std::unique_ptr<RecordChunk> Scheduler::AcquireChunk() {
#ifdef __APPLE__
    if (async_submit) {
        // A bounded budget of chunks (queued, held by this thread, being executed): over it,
        // this thread's full chunks go to the recorder and it waits for one to come back.
        std::unique_lock lk{recorder_mutex};
        if (free_chunks.empty() && live_chunks >= MaxChunks) {
            lk.unlock();
            QueueFullChunks();
            lk.lock();
            BbStats::WaitTimer timer{BbStats::sync_recording_ns};
            BB_SPLIT_SCOPE(KRecorderSync);
            recorder_space_cv.wait(lk, [this] { return !free_chunks.empty(); });
        }
        if (free_chunks.empty()) {
            ++live_chunks;
            return std::make_unique<RecordChunk>();
        }
        auto chunk = std::move(free_chunks.back());
        free_chunks.pop_back();
        return chunk;
    }
#endif
    std::scoped_lock lk{recorder_mutex};
    if (free_chunks.empty()) {
        return std::make_unique<RecordChunk>();
    }
    auto chunk = std::move(free_chunks.back());
    free_chunks.pop_back();
    return chunk;
}

#ifdef __APPLE__
namespace {
/// Timelines whose submissions are asynchronous, by semaphore (WaitDependencies).
std::mutex async_timelines_mutex;
std::array<std::pair<VkSemaphore, Scheduler*>, 4> async_timelines{};
} // namespace

void Scheduler::RegisterAsyncTimeline(vk::Semaphore semaphore, Scheduler* owner) {
    std::scoped_lock lk{async_timelines_mutex};
    for (auto& entry : async_timelines) {
        if (!entry.first || entry.second == owner) {
            entry = {semaphore, owner};
            return;
        }
    }
    UNREACHABLE_MSG("More than {} asynchronously submitting schedulers", async_timelines.size());
}

void Scheduler::UnregisterAsyncTimeline(Scheduler* owner) {
    std::scoped_lock lk{async_timelines_mutex};
    for (auto& entry : async_timelines) {
        if (entry.second == owner) {
            entry = {};
        }
    }
}

Scheduler* Scheduler::AsyncTimelineOwner(vk::Semaphore semaphore) {
    std::scoped_lock lk{async_timelines_mutex};
    for (const auto& entry : async_timelines) {
        if (entry.first && entry.first == static_cast<VkSemaphore>(semaphore)) {
            return entry.second;
        }
    }
    return nullptr;
}

void Scheduler::QueueFullChunks() {
    bool wake;
    {
        std::scoped_lock lk{recorder_mutex};
        for (auto& chunk : full_chunks) {
            recorder_queue.push_back({std::move(chunk), nullptr});
        }
        wake = recorder_sleeping;
        queued_chunks.store(recorder_queue.size(), std::memory_order_release);
    }
    full_chunks.clear();
    if (wake) {
        recorder_cv.notify_one();
    }
}
#endif

void Scheduler::SignalAfterHostCopies(std::function<void()> signal) {
    if (!IsRecordingDeferred()) {
        WaitHostCopies();
        signal();
        return;
    }
    BbCopy::FlushBatch();
    deferred_signals_issued.fetch_add(1, std::memory_order_relaxed);
#ifdef __APPLE__
    recorder_signals.Issue();
    Record([signal = std::move(signal), done = deferred_signals_done,
            recorder_done = recorder_signals.done](vk::CommandBuffer) mutable {
        BbCopy::AfterCopies([signal = std::move(signal), done = std::move(done),
                             recorder_done = std::move(recorder_done)] {
            signal();
            recorder_done->fetch_add(1, std::memory_order_release);
            done->fetch_add(1, std::memory_order_release);
        });
    });
#else
    Record([signal = std::move(signal), done = deferred_signals_done](vk::CommandBuffer) mutable {
        BbCopy::AfterCopies([signal = std::move(signal), done = std::move(done)] {
            signal();
            done->fetch_add(1, std::memory_order_release);
        });
    });
#endif
    KickRecording(true);
}

#ifdef __APPLE__
bool Scheduler::LabelsAtGpuComplete() {
    static const BbLabels::RunMode run = [] {
        const auto mode = BbLabels::ParseRunMode(std::getenv("BB_LABELS_AT_GPU_COMPLETE"));
        std::fprintf(stderr, "Labels (EOP/EOS/RELEASE_MEM): %s\n",
                     mode == BbLabels::RunMode::AtGpuComplete
                         ? "at GPU completion for the whole run (BB_LABELS_AT_GPU_COMPLETE=1)"
                     : mode == BbLabels::RunMode::AtSubmit
                         ? "at submit for the whole run (BB_LABELS_AT_GPU_COMPLETE=0, diagnosis)"
                         : "at GPU completion (default; toggle bit 59 writes them at submit)");
        return mode;
    }();
    // Toggle bit 59 (BB_TOGGLE_FILE) switches it live; Liverpool keeps the order at a switch.
    return BbLabels::AtGpuComplete(run, BbToggle::Disabled(BbToggle::LabelsAtSubmit));
}

void Scheduler::FlushLabels(bool idle) {
    if (!labels_unflushed.load(std::memory_order_relaxed)) {
        return;
    }
    const auto age = std::chrono::steady_clock::now() - unflushed_since;
    if (!idle && age < LabelBatchAge) {
        return;
    }
#ifdef __APPLE__
    BbSplit::NoteLabelBatch(
        std::chrono::duration_cast<std::chrono::nanoseconds>(age).count());
    BbSplit::FlushReason reason{idle ? BbSplit::FBatchIdle : BbSplit::FBatchAge};
#endif
    Flush(); // asserts it signals unflushed_tick
}

void Scheduler::SignalAtGpuCompletion(std::function<void()> signal, bool batch) {
    // Issue-to-write age, the delay the guest sees on a label (parse split's Flushes line).
    signal = [issued = std::chrono::steady_clock::now(), signal = std::move(signal)] {
        signal();
        BbSplit::NoteLabelWritten(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      std::chrono::steady_clock::now() - issued)
                                      .count());
    };
    const u64 tick = CurrentTick(); // the next submission signals this value
    // Issued now, in stream order; the writer takes labels only in this order, whichever
    // becomes ready first (deferred ones after the copy threads, direct ones at once).
    const u64 sequence = labels.Issue();
    completion_labels.Issue();
    last_label_tick.store(tick, std::memory_order_relaxed);
    deferred_signals_issued.fetch_add(1, std::memory_order_relaxed);
    std::call_once(label_thread_once, [this] {
        label_thread = std::jthread([this](std::stop_token stoken) { LabelThread(stoken); });
    });
    if (!IsRecordingDeferred()) {
#ifdef __APPLE__
        BbSplit::FlushReason reason{BbSplit::FLabel};
#endif
        const u64 submitted = Flush();
        ASSERT_MSG(submitted == tick,
                   "macOS: a label's submission signals timeline value {}, its writer waits for {}",
                   submitted, tick);
        WaitHostCopies(); // the guest-memory copies issued before it
        labels.Ready(sequence, tick, std::move(signal));
        return;
    }
    BbCopy::FlushBatch();
    Record([this, tick, sequence, signal = std::move(signal)](vk::CommandBuffer) mutable {
        // After the copies queued before it; the writer then waits for `tick`.
        BbCopy::AfterCopies([this, tick, sequence, signal = std::move(signal)]() mutable {
            labels.Ready(sequence, tick, std::move(signal));
        });
    });
    if (batch) {
        // Submitted by the next submission, whoever makes it: that one signals `tick`
        // (SubmitExecution asserts it). FlushLabels makes sure it comes soon.
        if (!labels_unflushed.load(std::memory_order_relaxed)) {
            unflushed_tick = tick;
            unflushed_first_label = completion_labels.Issued();
            unflushed_since = std::chrono::steady_clock::now();
            labels_unflushed.store(true, std::memory_order_release);
        }
        ASSERT_MSG(unflushed_tick == tick,
                   "macOS: batched labels span timeline values {} and {} without a submission",
                   unflushed_tick, tick);
        return;
    }
#ifdef __APPLE__
    BbSplit::FlushReason reason{BbSplit::FLabel};
#endif
    const u64 submitted = Flush();
    // The writer waits for `tick`: it must be the submission holding the label and the work
    // recorded before it (no other submission came between).
    ASSERT_MSG(submitted == tick,
               "macOS: a label's submission signals timeline value {}, its writer waits for {}",
               submitted, tick);
}

bool Scheduler::WaitLabelTick(u64 tick, const std::stop_token& stoken) {
    // Waited in slices so that a stop (exit, possibly after a device loss) is never stuck here;
    // a failed wait stops loudly.
    work_semaphore.Refresh();
    while (!work_semaphore.IsFree(tick)) {
        if (stoken.stop_requested()) {
            return false;
        }
        const vk::Semaphore semaphore = work_semaphore.Handle();
        const vk::SemaphoreWaitInfo wait_info = {
            .semaphoreCount = 1,
            .pSemaphores = &semaphore,
            .pValues = &tick,
        };
        const vk::Result result =
            instance.GetDevice().waitSemaphores(&wait_info, 10'000'000); // 10 ms
        ASSERT_MSG(result == vk::Result::eSuccess || result == vk::Result::eTimeout,
                   "macOS: the label writer's wait for timeline value {} failed: {}", tick,
                   vk::to_string(result));
        work_semaphore.Refresh();
    }
    return true;
}

void Scheduler::LabelThread(std::stop_token stoken) {
    Common::SetCurrentThreadName("shadPS4:GpuLabelWriter");
    u64 dropped = 0;
    while (auto label = labels.Next(stoken)) {
        if (!WaitLabelTick(label->tick, stoken)) {
            ++dropped; // taken, its work not complete
            break;
        }
        label->write();
        completion_labels.Complete();
        deferred_signals_done->fetch_add(1, std::memory_order_release);
    }
    // Stopping (exit): labels not written are dropped, and counted as done so that nothing is
    // left waiting for them (WaitDeferredSignals).
    dropped += labels.Outstanding();
    if (dropped) {
        completion_labels.Complete(dropped);
        deferred_signals_done->fetch_add(dropped, std::memory_order_release);
        std::fprintf(stderr, "Labels at GPU completion: %llu labels not written at shutdown\n",
                     static_cast<unsigned long long>(dropped));
    }
}
#endif

#ifdef __APPLE__
bool Scheduler::WaitQuiescentForCapture(std::chrono::milliseconds limit, std::string& why) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while ((num_pending_ops.load(std::memory_order_acquire) || PriorityOperationsOutstanding() ||
            DeferredSignalsOutstanding()) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::string outstanding;
    if (const u32 ops = num_pending_ops.load(std::memory_order_acquire)) {
        outstanding += fmt::format("{} deferred operations; ", ops);
    }
    if (const u64 ops = PriorityOperationsOutstanding()) {
        outstanding += fmt::format("{} priority operations not finished; ", ops);
    }
    if (const u64 signals = DeferredSignalsOutstanding()) {
        outstanding += fmt::format("{} deferred signals (labels) not written; ", signals);
    }
    if (outstanding.empty()) {
        return true;
    }
    why = outstanding.substr(0, outstanding.size() - 2);
    return false;
}
#endif

#ifdef __APPLE__
void Scheduler::WaitLabelsThrough(u64 through) {
    // The label writer completes labels in issue order: a prefix (bbport_write_order.h).
    if (completion_labels.CompleteThrough(through)) {
        return;
    }
    const auto start = std::chrono::steady_clock::now();
    {
        BbStats::WaitTimer timer{BbStats::deferred_signals_wait_ns};
        BbSplit::Scope split_scope{BbSplit::deferred_site};
        KickRecording(true);
        while (!completion_labels.CompleteThrough(through)) {
            BbCopy::WaitAsync();
            std::this_thread::yield();
        }
    }
    BbSplit::NoteOrderedWriteWaited(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::steady_clock::now() - start)
                                        .count());
}
#endif

void Scheduler::WaitDeferredSignals() {
    const u64 issued = deferred_signals_issued.load(std::memory_order_relaxed);
    if (deferred_signals_done->load(std::memory_order_acquire) >= issued ||
        BbToggle::Disabled(BbToggle::OrderedGuestWrites)) {
        return;
    }
    BbStats::WaitTimer timer{BbStats::deferred_signals_wait_ns};
#ifdef __APPLE__
    BbSplit::Scope split_scope{BbSplit::deferred_site}; // by site (RunWriteData, SignalFlip)
#endif
    KickRecording(true);
    while (deferred_signals_done->load(std::memory_order_acquire) < issued) {
        // Helps the copy threads the signals wait for.
        BbCopy::WaitAsync();
        std::this_thread::yield();
    }
}

#ifdef __APPLE__
void Scheduler::WaitRecorderSignals() {
    // Its own counts (bbport_deferred_counts.h), not a difference of the total and the labels.
    const u64 issued = recorder_signals.Issued();
    if (recorder_signals.CompleteThrough(issued) ||
        BbToggle::Disabled(BbToggle::OrderedGuestWrites)) {
        return;
    }
    BB_SPLIT_SCOPE(KRecorderSignals);
    KickRecording(true);
    while (!recorder_signals.CompleteThrough(issued)) {
        BbCopy::WaitAsync();
        std::this_thread::yield();
    }
}
#endif

void Scheduler::WaitHostCopies() {
    if (host_copies_done.load(std::memory_order_acquire) < host_copies_issued) {
        BbStats::WaitTimer timer{BbStats::host_copy_record_wait_ns};
        BB_SPLIT_SCOPE(KHostCopies);
        BbStats::host_copy_waits.fetch_add(1, std::memory_order_relaxed);
        KickRecording(true);
        while (host_copies_done.load(std::memory_order_acquire) < host_copies_issued) {
            std::this_thread::yield();
        }
    }
    BbStats::WaitTimer timer{BbStats::copy_threads_wait_ns};
    BbCopy::WaitAsync();
}

void Scheduler::KickRecording(bool force) {
    if (!recording_threaded) {
        return;
    }
    // Callers kick where nobody holds the raw command buffer: deferral resumes.
    direct_mode = false;
    // Batches of tens of KiB keep the queue handoff cheap relative to the work it carries.
    if (!force && full_chunks.empty() && record_chunk->Size() < 32 * 1024) {
        return;
    }
    if (full_chunks.empty() && record_chunk->Empty()) {
        return;
    }
    bool wake;
    {
        std::scoped_lock lk{recorder_mutex};
        for (auto& chunk : full_chunks) {
#ifdef __APPLE__
            recorder_queue.push_back({std::move(chunk), nullptr});
#else
            recorder_queue.push_back(std::move(chunk));
#endif
        }
        if (!record_chunk->Empty()) {
#ifdef __APPLE__
            recorder_queue.push_back({std::move(record_chunk), nullptr});
#else
            recorder_queue.push_back(std::move(record_chunk));
#endif
        }
        wake = recorder_sleeping;
        queued_chunks.store(recorder_queue.size(), std::memory_order_release);
    }
    full_chunks.clear();
    // A busy recorder picks the new chunks up by itself: waking it is a syscall per draw.
    if (wake) {
        recorder_cv.notify_one();
    }
    if (!record_chunk) {
        record_chunk = AcquireChunk();
    }
}

void Scheduler::SyncRecording() {
    if (!recording_threaded) {
        return;
    }
    KickRecording(true);
    BbStats::WaitTimer timer{BbStats::sync_recording_ns};
    BB_SPLIT_SCOPE(KRecorderSync);
    std::unique_lock lk{recorder_mutex};
    recorder_idle_cv.wait(lk, [this] { return recorder_queue.empty() && !recorder_busy; });
}

void Scheduler::RecorderThread(std::stop_token stoken) {
    Common::SetCurrentThreadName("bb:VkRecorder");
#ifdef __APPLE__
    recording_for = this; // this thread records for this scheduler (StopOnRecorderThread)
    BbHotThreadQos::Start("Vulkan recording");
#endif
#ifdef __APPLE__
    BbSplit::Attach(BbSplit::recorder, BbSplit::RIdle);
#endif
    while (true) {
        // Spin briefly before sleeping: the next chunk usually follows within microseconds,
        // and a sleeping recorder costs the GPU thread a wake-up syscall per kick. With few
        // hardware threads (Steam Deck: 8) the spin would take time from guest threads.
        static const auto spin_time = std::chrono::microseconds(
            BbThreads::Available() >= 12 ? 200 : 20);
        const auto spin_until = std::chrono::steady_clock::now() + spin_time;
        for (u32 spins = 1; queued_chunks.load(std::memory_order_acquire) == 0; ++spins) {
            __builtin_ia32_pause();
            // The clock is read every 256 pauses, not per iteration.
            if (!(spins & 255) &&
                (stoken.stop_requested() || std::chrono::steady_clock::now() >= spin_until)) {
                break;
            }
        }
        std::unique_ptr<RecordChunk> chunk;
#ifdef __APPLE__
        std::unique_ptr<SubmitPacket> submit;
#endif
        {
            std::unique_lock lk{recorder_mutex};
            recorder_sleeping = true;
            recorder_cv.wait(lk, stoken, [this] { return !recorder_queue.empty(); });
            recorder_sleeping = false;
            if (recorder_queue.empty()) {
                return; // stop requested
            }
#ifdef __APPLE__
            chunk = std::move(recorder_queue.front().chunk);
            submit = std::move(recorder_queue.front().submit);
#else
            chunk = std::move(recorder_queue.front());
#endif
            recorder_queue.pop_front();
            queued_chunks.store(recorder_queue.size(), std::memory_order_release);
            recorder_busy = true;
        }
#ifdef __APPLE__
        if (submit) {
            // A strict boundary: the commands after it are recorded into the buffer it begins.
            SubmitFromRecorder(*submit);
            std::scoped_lock lk{recorder_mutex};
            --queued_submits;
            recorder_busy = false;
            if (recorder_queue.empty()) {
                recorder_idle_cv.notify_all();
            }
            recorder_space_cv.notify_all();
            continue;
        }
#endif
        // current_cmdbuf only changes after SyncRecording(), which waits for this thread.
        {
#ifdef __APPLE__
            BbPerf::RecorderTimer perf_timer;
            BbSplit::Mark(BbSplit::RExecute);
#endif
            chunk->Execute(current_cmdbuf);
#ifdef __APPLE__
            BbSplit::Mark(BbSplit::RIdle);
#endif
        }
        {
            std::scoped_lock lk{recorder_mutex};
            free_chunks.push_back(std::move(chunk));
            recorder_busy = false;
            if (recorder_queue.empty()) {
                recorder_idle_cv.notify_all();
            }
#ifdef __APPLE__
            recorder_space_cv.notify_all();
#endif
        }
    }
}

u64 Scheduler::Flush(SubmitInfo& info) {
    // When flushing, we only send data to the driver; no waiting is necessary.
#ifdef __APPLE__
    if (closed) {
        return CurrentTick() - 1; // Close submitted everything and waited; nothing is open
    }
    BB_SPLIT_SCOPE(KFlush);
    // Every wait is in `info` once it is sealed (on_submit, WaitBeforeNextSubmit's waits);
    // ordered against other schedulers outside submit_mutex.
    const u64 value = SealSubmission(info);
    WaitDependencies(info, value);
    if (async_submit && !closing && IsRecordingDeferred()) {
        return EnqueueSubmission(info, value);
    }
    // Synchronous (direct recording, toggle): queued packets need submit_mutex, which
    // SubmitExecution holds while it syncs: submit them first.
    SyncRecording();
    return SubmitExecution(info, value);
#else
    BB_SPLIT_SCOPE(KFlush);
    return SubmitExecution(info);
#endif
}

u64 Scheduler::Flush() {
    SubmitInfo info{};
    return Flush(info);
}

void Scheduler::WaitBeforeNextSubmit(vk::Semaphore semaphore, u64 value) {
    std::scoped_lock lk{submit_mutex};
    next_submit_waits.emplace_back(semaphore, value);
}

void Scheduler::Finish() {
    // When finishing, we need to wait for the submission to have executed on the device.
#ifdef __APPLE__
    if (closed) {
        return; // Close waited for everything submitted; recording since then stops
    }
    BbSplit::FlushReason reason{BbSplit::FFinish};
#endif
    SubmitInfo info{};
#ifdef __APPLE__
    // Through Flush: enqueued like any submission, then waited for (Wait accepts a value still
    // queued for vkQueueSubmit).
    Wait(Flush(info));
#else
    const u64 presubmit_tick = CurrentTick();
    SubmitExecution(info);
    Wait(presubmit_tick);
#endif
}

#ifdef __APPLE__
void Scheduler::Close() {
    if (closed || closing) {
        return;
    }
    SyncRecording();
    EndRendering();
    closing = true;
    SubmitInfo info{};
    // The submit callback may still record (pending barriers); its waits are ordered as any.
    const u64 tick = SealSubmission(info);
    WaitDependencies(info, tick);
    SyncRecording();
    SubmitExecution(info, tick);
    closed = true;
    BB_SPLIT_SCOPE(KGpuTick);
    work_semaphore.Wait(tick);
    PopPendingOperations();
}

void Scheduler::StopRecordingAfterClose() {
    std::fputs("STOP: recording after Scheduler::Close\n", stderr);
    ASSERT_MSG(false, "Recording after Scheduler::Close");
    std::abort();
}
#endif

void Scheduler::WaitPreviousRetired() {
    const u64 tick = CurrentTick();
    if (tick > 1) {
        Wait(tick - 1);
    }
}

#ifdef __APPLE__
void Scheduler::WaitDependencies(const SubmitInfo& info, u64 own_value) {
    StopOnRecorderThread("WaitDependencies");
    for (u32 i = 0; i < info.num_wait_semas; ++i) {
        Scheduler* const owner = AsyncTimelineOwner(info.wait_semas[i]);
        if (!owner) {
            continue; // binary or external semaphores: not ordered by our queues
        }
        const u64 value = info.wait_ticks[i];
        if (owner == this) {
            // An earlier value of this timeline is ahead in this queue already (FIFO); this
            // submission's own or a later one can never be signalled before it.
            if (value >= own_value) {
                std::fprintf(stderr,
                             "STOP: a submission waits on its own scheduler's value %llu (open "
                             "value %llu): never signalled before it\n",
                             static_cast<unsigned long long>(value),
                             static_cast<unsigned long long>(own_value));
                ASSERT_MSG(false, "A submission waits on its own scheduler's value {}", value);
            }
            continue;
        }
        // The proven dependency graph: synchronous schedulers (present, flip) wait on the
        // asynchronous one (draw). An asynchronous scheduler waiting on another could cycle.
        if (async_submit) {
            std::fputs("STOP: an asynchronously submitting scheduler waits on another one's "
                       "timeline (not a proven acyclic edge)\n",
                       stderr);
            ASSERT_MSG(false, "Async scheduler waits on another async timeline");
        }
        owner->WaitQueueSubmitted(value);
    }
}

void Scheduler::StopOnRecorderThread(const char* what) const {
    if (recording_for == this) {
        std::fprintf(stderr,
                     "STOP: %s on the scheduler's own recording thread (it would wait for "
                     "itself)\n",
                     what);
        ASSERT_MSG(false, "{} on the recording thread", what);
    }
}

void Scheduler::WaitQueueSubmitted(u64 tick) {
    StopOnRecorderThread("WaitQueueSubmitted");
    if (tick >= CurrentTick()) {
        std::fprintf(stderr,
                     "STOP: WaitQueueSubmitted(%llu) for a value not assigned yet (open value "
                     "%llu)\n",
                     static_cast<unsigned long long>(tick),
                     static_cast<unsigned long long>(CurrentTick()));
        ASSERT_MSG(false, "WaitQueueSubmitted({}) for a value not assigned yet", tick);
    }
    if (submitted_through.load(std::memory_order_acquire) >= tick) {
        return;
    }
    BbStats::WaitTimer timer{BbStats::tick_wait_ns};
    BB_SPLIT_SCOPE(KGpuTick);
    std::unique_lock lk{recorder_mutex};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    const bool ready = recorder_space_cv.wait_until(lk, deadline, [&] {
        return submitted_through.load(std::memory_order_acquire) >= tick || submit_failed ||
               closed;
    });
    if (submitted_through.load(std::memory_order_acquire) >= tick) {
        return;
    }
    std::fprintf(stderr,
                 "STOP: submission %llu was not handed to vkQueueSubmit (%s; enqueued through "
                 "%llu, submitted through %llu)\n",
                 static_cast<unsigned long long>(tick),
                 !ready ? "10 s" : submit_failed ? "submission failed" : "scheduler closed",
                 static_cast<unsigned long long>(enqueued_through.load()),
                 static_cast<unsigned long long>(submitted_through.load()));
    ASSERT_MSG(false, "Submission {} was never handed to vkQueueSubmit", tick);
}

u64 Scheduler::SealSubmission(SubmitInfo& info) {
    const u64 signal_value = work_semaphore.NextTick();
    // Batched labels (SignalAtGpuCompletion) were recorded for exactly this submission.
    if (labels_unflushed.exchange(false, std::memory_order_acq_rel)) {
        ASSERT_MSG(signal_value == unflushed_tick,
                   "macOS: batched labels wait for timeline value {}, their submission signals {}",
                   unflushed_tick, signal_value);
    }
    if (on_submit) {
        on_submit(info); // records into this submission (the rasterizer's barriers)
    }
    for (const auto& [semaphore, value] : next_submit_waits) {
        info.AddWait(semaphore, value, vk::PipelineStageFlagBits::eAllCommands);
    }
    next_submit_waits.clear();
    return signal_value;
}

u64 Scheduler::EnqueueSubmission(SubmitInfo& info, u64 signal_value) {
    auto packet = std::make_unique<SubmitPacket>();
    {
        if (flush_census) {
            // After on_submit's barrier flush, as in SubmitExecution: a pass still open is
            // ended by this submission (the pass census's "submit" enders).
            BbSplit::NoteFlush(is_rendering);
        }
        EndRenderingFor(BbPassCensus::Why::Submit);
        packet->info = info;
        packet->signal_value = signal_value;
        packet->next_tick = signal_value + 1;
        packet->copies_through = host_copies_issued;
        packet->perf_phase = perf_phase;
        packet->perf_frame = perf_frame;
    }
    {
        std::unique_lock lk{recorder_mutex};
        if (queued_submits >= MaxQueuedSubmits) {
            BbStats::WaitTimer timer{BbStats::sync_recording_ns};
            BB_SPLIT_SCOPE(KRecorderSync);
            recorder_space_cv.wait(lk, [this] { return queued_submits < MaxQueuedSubmits; });
        }
        // The submission's commands, then the packet: a strict boundary (the commands after it
        // are recorded into the buffer it begins).
        for (auto& chunk : full_chunks) {
            recorder_queue.push_back({std::move(chunk), nullptr});
        }
        if (!record_chunk->Empty()) {
            recorder_queue.push_back({std::move(record_chunk), nullptr});
        }
        recorder_queue.push_back({nullptr, std::move(packet)});
        ++queued_submits;
        enqueued_through.store(signal_value, std::memory_order_release);
        queued_chunks.store(recorder_queue.size(), std::memory_order_release);
    }
    full_chunks.clear();
    // Always woken: waits on host copies no longer kick the recorder themselves (PM4).
    recorder_cv.notify_one();
    if (!record_chunk) {
        record_chunk = AcquireChunk();
    }
    direct_mode = false;
    recorded_since_begin = false; // the recorder begins the next buffer
    dynamic_state.Invalidate();   // which the next commands are recorded into
    PopPendingOperations();
    return signal_value;
}

void Scheduler::SubmitFromRecorder(SubmitPacket& packet) {
    // The recorder's "queue submit": end, the host-copy wait (its own scope), vkQueueSubmit
    // (MoltenVK encodes the command buffer here), commit and begin of the next buffer.
    BB_SPLIT_SCOPE(KFlush);
    if (perf) perf->End(current_cmdbuf);
    Check(current_cmdbuf.end());
    // The host copies this submission reads: their dispatch commands were recorded before the
    // packet, so they have run (FIFO); wait for the copies themselves. AfterCopies also covers
    // copies the owner issued after the packet (BbCopy::Async from ObtainBuffer): a harmless
    // over-wait. No copy task waits on a GPU submission (they copy guest memory into staging,
    // docs/macos/async-submission.md), so this cannot cycle; 10 s is a hang detector.
    ASSERT_MSG(host_copies_done.load(std::memory_order_acquire) >= packet.copies_through,
               "Submission {}: host copy {} not dispatched before its submit (done {})",
               packet.signal_value, packet.copies_through, host_copies_done.load());
    {
        auto copies_done = std::make_shared<std::atomic<bool>>(false);
        BbCopy::AfterCopies([copies_done] { copies_done->store(true, std::memory_order_release); });
        if (!copies_done->load(std::memory_order_acquire)) {
            BbStats::WaitTimer timer{BbStats::copy_threads_wait_ns};
            BB_SPLIT_SCOPE(KHostCopies);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            for (u32 spins = 1; !copies_done->load(std::memory_order_acquire); ++spins) {
                std::this_thread::yield();
                if (!(spins & 255) && std::chrono::steady_clock::now() > deadline) {
                    std::fprintf(stderr,
                                 "STOP: the host copies awaited by submission %llu have not "
                                 "finished in 10 s (a hung copy task)\n",
                                 static_cast<unsigned long long>(packet.signal_value));
                    ASSERT_MSG(false, "Host copies of submission {} did not finish",
                               packet.signal_value);
                }
            }
        }
    }
    SubmitInfo& info = packet.info;
    info.AddSignal(work_semaphore.Handle(), packet.signal_value);
    const vk::TimelineSemaphoreSubmitInfo timeline_si = {
        .waitSemaphoreValueCount = info.num_wait_semas,
        .pWaitSemaphoreValues = info.wait_ticks.data(),
        .signalSemaphoreValueCount = info.num_signal_semas,
        .pSignalSemaphoreValues = info.signal_ticks.data(),
    };
    const vk::SubmitInfo submit_info = {
        .pNext = &timeline_si,
        .waitSemaphoreCount = info.num_wait_semas,
        .pWaitSemaphores = info.wait_semas.data(),
        .pWaitDstStageMask = info.wait_stages.data(),
        .commandBufferCount = 1U,
        .pCommandBuffers = &current_cmdbuf,
        .signalSemaphoreCount = info.num_signal_semas,
        .pSignalSemaphores = info.signal_semas.data(),
    };
    vk::Result submit_result;
    {
        std::scoped_lock lk{submit_mutex};
        submit_result = instance.GetGraphicsQueue().submit(submit_info, info.fence);
    }
    if (submit_result != vk::Result::eSuccess) {
        {
            std::scoped_lock lk{recorder_mutex};
            submit_failed = true;
        }
        recorder_space_cv.notify_all();
        ASSERT_MSG(false, "Submission {} failed: {}", packet.signal_value,
                   vk::to_string(submit_result));
    }
    if (perf) perf->Submitted(packet.signal_value);
    work_semaphore.Refresh();
    {
        // Under the mutex WaitQueueSubmitted checks its predicate with: no lost wakeup.
        std::scoped_lock lk{recorder_mutex};
        submitted_through.store(packet.signal_value, std::memory_order_release);
    }
    recorder_space_cv.notify_all();
    // The next buffer, stamped for the submission that will use it.
    current_cmdbuf = command_pool.Commit(packet.next_tick);
    recording_tick.store(packet.next_tick, std::memory_order_release);
    Check(current_cmdbuf.begin(vk::CommandBufferBeginInfo{
        .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
    }));
    if (perf) perf->Begin(current_cmdbuf, packet.perf_phase, packet.perf_frame);
}
#endif

void Scheduler::Wait(u64 tick) {
#ifdef __APPLE__
    StopOnRecorderThread("Scheduler::Wait");
#endif
    if (tick >= work_semaphore.CurrentTick()) {
        // Make sure we are not waiting for the current tick without signalling
        SubmitInfo info{};
        Flush(info);
    }
    BbStats::WaitTimer timer{BbStats::tick_wait_ns};
    BB_SPLIT_SCOPE(KGpuTick);
    work_semaphore.Wait(tick);
}

void Scheduler::PopPendingOperations() {
    if (num_pending_ops.load(std::memory_order_acquire) == 0) {
        return; // every draw comes here
    }
    std::unique_lock lk(pending_ops_mutex);
    // bbport: this runs on every draw and dispatch. Querying the timeline semaphore is an
    // ioctl, so it is skipped when nothing waits and done once per 32 calls (~0.3 ms; reading
    // the clock per draw instead was itself a hot spot).
    if (pending_ops.empty()) {
        return;
    }
    if (!work_semaphore.IsFree(pending_ops.front().gpu_tick)) {
        if ((++pending_polls & 31) != 0 && !BbToggle::Disabled(BbToggle::PendingPollLimit)) {
            return;
        }
        work_semaphore.Refresh();
    }
    while (!pending_ops.empty() && work_semaphore.IsFree(pending_ops.front().gpu_tick)) {
        pending_ops.front().callback();
        pending_ops.pop();
        num_pending_ops.fetch_sub(1, std::memory_order_release);
    }
}

void Scheduler::AllocateWorkerCommandBuffers() {
    const vk::CommandBufferBeginInfo begin_info = {
        .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
    };

    current_cmdbuf = command_pool.Commit();
    Check(current_cmdbuf.begin(begin_info));
#ifdef __APPLE__
    recording_tick.store(CurrentTick(), std::memory_order_release);
#endif
#ifdef __APPLE__
    recorded_since_begin = false;
#endif
#ifdef __APPLE__
    if (perf) perf->Begin(current_cmdbuf, perf_phase, perf_frame);
#endif

    // Invalidate dynamic state so it gets applied to the new command buffer.
    dynamic_state.Invalidate();

#if TRACY_GPU_ENABLED
    auto* profiler_ctx = instance.GetProfilerContext();
    if (profiler_ctx) {
        static const auto scope_loc =
            GPU_SCOPE_LOCATION("Guest Frame", MarkersPalette::GpuMarkerColor);
        new (profiler_scope) tracy::VkCtxScope{profiler_ctx, &scope_loc, current_cmdbuf, true};
    }
#endif
}

u64 Scheduler::SubmitExecution(SubmitInfo& info, u64 sealed_value) {
    std::scoped_lock lk{submit_mutex};
#ifdef __APPLE__
    // Sealed by the caller (SealSubmission: value, labels check, on_submit, waits), or here.
    const u64 signal_value = sealed_value ? sealed_value : SealSubmission(info);
#else
    const u64 signal_value = work_semaphore.NextTick();
#endif

#if TRACY_GPU_ENABLED
    auto* profiler_ctx = instance.GetProfilerContext();
    if (profiler_ctx) {
        profiler_scope->~VkCtxScope();
        TracyVkCollect(profiler_ctx, current_cmdbuf);
    }
#endif

#ifndef __APPLE__
    if (on_submit) {
        on_submit(info);
    }
    for (const auto& [semaphore, value] : next_submit_waits) {
        info.AddWait(semaphore, value, vk::PipelineStageFlagBits::eAllCommands);
    }
    next_submit_waits.clear();
#endif

#ifdef __APPLE__
    if (flush_census) {
        // After on_submit's barrier flush: a pass still open is ended by this submission (the
        // render pass census's "submit" enders).
        BbSplit::NoteFlush(is_rendering);
    }
    EndRenderingFor(BbPassCensus::Why::Submit);
#else
    EndRendering();
#endif
    if (auto* profiler = GpuProfiler::Get(); profiler && profiler->Records(this)) {
        // Until the next submission's first timestamp: mostly the GPU waiting for it.
        profiler->Mark(0x5B317ull, [] { return std::string{"(between submissions: GPU idle)"}; });
    }
    SyncRecording();
    // Guest memory copies into staging read by this submission (copy threads).
    WaitHostCopies();
#ifdef __APPLE__
    if (perf) perf->End(current_cmdbuf);
#endif
    Check(current_cmdbuf.end());

    const vk::Semaphore timeline = work_semaphore.Handle();
    info.AddSignal(timeline, signal_value);

    const vk::TimelineSemaphoreSubmitInfo timeline_si = {
        .waitSemaphoreValueCount = info.num_wait_semas,
        .pWaitSemaphoreValues = info.wait_ticks.data(),
        .signalSemaphoreValueCount = info.num_signal_semas,
        .pSignalSemaphoreValues = info.signal_ticks.data(),
    };

    const vk::SubmitInfo submit_info = {
        .pNext = &timeline_si,
        .waitSemaphoreCount = info.num_wait_semas,
        .pWaitSemaphores = info.wait_semas.data(),
        .pWaitDstStageMask = info.wait_stages.data(),
        .commandBufferCount = 1U,
        .pCommandBuffers = &current_cmdbuf,
        .signalSemaphoreCount = info.num_signal_semas,
        .pSignalSemaphores = info.signal_semas.data(),
    };

    ImGui::Core::TextureManager::Submit();
    auto submit_result = instance.GetGraphicsQueue().submit(submit_info, info.fence);
    ASSERT_MSG(submit_result != vk::Result::eErrorDeviceLost, "Device lost during submit");
#ifdef __APPLE__
    if (perf) {
        Check(submit_result);
        perf->Submitted(signal_value);
    }
#endif

    work_semaphore.Refresh();
#ifdef __APPLE__
    // Synchronous submission (no recording thread, direct recording, Close): the same
    // watermarks the asynchronous path keeps.
    {
        std::scoped_lock lk{recorder_mutex};
        enqueued_through.store(signal_value, std::memory_order_release);
        submitted_through.store(signal_value, std::memory_order_release);
    }
    recorder_space_cv.notify_all();
    if (closing) {
        PopPendingOperations();
        return signal_value;
    }
#endif
    AllocateWorkerCommandBuffers();

    // Apply pending operations
    PopPendingOperations();
    return signal_value;
}

#ifdef __APPLE__
void Scheduler::InitPerf(bool present) {
    if (!BbPerf::Enabled()) return;
    ASSERT(!perf);
    perf = std::make_unique<BbPerf::GpuStream>(instance.GetPhysicalDevice(), instance.GetDevice(),
        instance.GetGraphicsQueueFamilyIndex(), work_semaphore.Handle(), present);
    perf_phase = present ? BbPerf::Phase::Present : BbPerf::Phase::Scene;
    perf->Begin(current_cmdbuf, perf_phase, perf_frame);
}
// Labels the following work. MoltenVK timestamps inside a command buffer do not attribute work,
// so with BB_PERF_PHASES a new phase starts a new submission; otherwise only the label changes.
// Called by the scheduler's current owner (draw-pipe stage B, or stage A after the drain), the
// same thread that already submits from these hooks (the upscaler's Finish()).
void Scheduler::PerfMark(BbPerf::Phase phase, u64 frame) {
    if (!perf) return;
    const bool boundary = phase != perf_phase || (frame && frame != perf_frame);
    perf_phase = phase;
    if (frame) perf_frame = frame;
    if (boundary && BbPerf::Phases()) {
        EndRendering();
        Flush();
    }
}
#endif

void Scheduler::PriorityPendingOpsThread(std::stop_token stoken) {
    Common::SetCurrentThreadName("shadPS4:GpuSchedPriorityPendingOpsRunner");

    while (!stoken.stop_requested()) {
        PendingOp op;
        {
            std::unique_lock lk(priority_pending_ops_mutex);
            priority_pending_ops_cv.wait(lk, stoken,
                                         [this] { return !priority_pending_ops.empty(); });
            if (stoken.stop_requested()) {
                break;
            }

            op = std::move(priority_pending_ops.front());
            priority_pending_ops.pop();
        }

        work_semaphore.Wait(op.gpu_tick);
        if (stoken.stop_requested()) {
            break;
        }

        op.callback();
#ifdef __APPLE__
        priority_ops_completed.fetch_add(1, std::memory_order_acq_rel);
#endif
    }
}

void DynamicState::Commit(const Instance& instance, const vk::CommandBuffer& cmdbuf) {
    BB_SPLIT_SCOPE(KDynamicState);
    // A null command buffer only updates the dirty flags, exactly as recording would.
    CommitWith(instance.IsDepthBoundsSupported(), instance.IsDynamicColorWriteMaskSupported(),
               instance.IsAttachmentFeedbackLoopLayoutSupported(), [&](auto&& command) {
                   if (cmdbuf) {
                       command(cmdbuf);
                   }
               });
}

} // namespace Vulkan
