// bbport (macOS): where the GPU-side threads' time goes. "Parse split" lines, every 600 frame
// ends (docs/macos/parse-split.md).
//
// Each instrumented thread's time is partitioned exactly. Every transition (a mark, a scope's
// start or end) first charges the time since the previous one to its owner: the innermost open
// scope (listed under the bucket open meanwhile), else the open bucket. Coroutine yields save and
// restore the open bucket (Suspend/Resume). A thread with no account (current == nullptr)
// records nothing.
//
// One writer per account (its thread); the reporter reads with relaxed loads, so a window is
// approximate at its edges (see Report below). Always on: a mark is one counter read and a few
// stores (none when the bucket is already open); a scope two reads.
#pragma once
#ifdef __APPLE__
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <mach/mach.h>
#include <pthread.h>
#include <x86intrin.h>

namespace BbSplit {

/// Buckets of the GPU command thread (stage A), by what it is doing between marks.
enum A : std::uint32_t {
    AIdle,       ///< waiting for guest submissions
    ALoop,       ///< the scheduler loop between queue tasks
    AHostCmds,   ///< host commands (flips, buffer syncs)
    ARegisters,  ///< SET_*_REG, CONTEXT_CONTROL, CLEAR_STATE, index state, other state packets
    ADraw,       ///< direct draws (pipeline selection, packet build)
    AIndirect,   ///< indirect draws
    ADispatch,   ///< dispatches
    ALabels,     ///< EVENT_WRITE(_EOP/_EOS), RELEASE_MEM, WRITE_DATA
    AWaitRegMem, ///< WAIT_REG_MEM
    AMemory,     ///< DMA_DATA, COPY_DATA, MEM_SEMAPHORE, ACQUIRE_MEM, COND_EXEC, other memory ops
    AConstEng,   ///< constant engine packets (CCB)
    AIndBuffer,  ///< INDIRECT_BUFFER
    ACompute,    ///< compute queue packets (ACB)
    AFrameEnd,   ///< frame end and GpuIdle (submit done, capture, flush)
    AOther,
    ACount
};
inline constexpr const char* a_names[ACount] = {
    "waiting for submissions", "scheduler loop", "host commands", "registers", "draws",
    "indirect draws", "dispatches", "labels", "WAIT_REG_MEM", "memory packets",
    "constant engine", "INDIRECT_BUFFER", "compute queues", "frame end and GpuIdle", "other"};

/// Buckets of the draw recording thread (stage B).
enum B : std::uint32_t {
    BIdle,        ///< no packet (spinning or asleep)
    BDecode,      ///< packet decode, register copy, consistency checks
    BTask,        ///< in-order tasks (labels, WRITE_DATA, DMA)
    BSpecial,     ///< FilterDraw (fast-clear elimination, resolve, depth copy)
    BDispatch,    ///< DispatchRecord / DispatchIndirectRecord
    BIndirect,    ///< DrawIndirectRecord
    BDrawSetup,   ///< DrawRecord before the render state (frame start hooks, GS emulation)
    BRenderState, ///< PrepareRenderState (targets)
    BBind,        ///< BindResources other than the two below
    BBuffers,     ///< BindBuffers
    BTextures,    ///< BindTextures (image lookup, refresh records)
    BBegin,       ///< BeginRendering
    BRecord,      ///< the rest of DrawRecord (dynamic state, draw recording, PostDraw)
    BAfter,       ///< after-packet hook
    BCount
};
inline constexpr const char* b_names[BCount] = {
    "no packet", "decode", "in-order tasks", "special draws", "dispatches", "indirect draws",
    "draw setup", "render state", "bind (other)", "bind buffers", "bind textures",
    "begin rendering", "record", "after packet"};

/// Buckets of the Vulkan recording thread.
enum R : std::uint32_t { RIdle, RExecute, RCount };
inline constexpr const char* r_names[RCount] = {"idle", "executing chunks"};

/// Scope kinds: waits, and named work inside a bucket.
enum K : std::uint32_t {
    KDrainConstRam,  ///< stage A waits for stage B: DUMP_CONST_RAM
    KDrainSubmitEnd, ///< stage A waits for stage B: queues ran dry (GpuIdle)
    KDrainRasterizer,///< stage A waits for stage B: OnSubmit, Flush, Finish
    KDrainPacket,    ///< stage A waits for stage B: a non-pipelined packet, host commands, compute
    KRingFull,       ///< stage A waits for space in the draw pipe
    KDeferred,       ///< waits for deferred signals (labels at GPU completion)
    KRecorderSignals,///< waits for recorder-side signal writes
    KRecorderSync,   ///< waits for the Vulkan recording thread to run dry
    KHostCopies,     ///< waits for host copies
    KGpuTick,        ///< waits for a GPU tick (fence)
    KVoLabel,        ///< waits for a presentation label
    KFlush,          ///< Scheduler::Flush (queue submit), less the waits inside
    // Recording internals (Opus-Renderer): work inside the buckets above.
    KImageRefresh,   ///< TextureCache::RefreshImage (re-read from guest memory: detile, upload)
    KTexelSync,      ///< BufferCache::SynchronizeMemoryFromImage (an image tiled into its memory)
    KPipelineWait,   ///< waits for a pipeline still being built (WaitBuiltSlow)
    KDescriptorBuild,///< Pipeline::BindResources: push constants, descriptor writes
    KDynamicState,   ///< DynamicState::Commit
    KBarrierFlush,   ///< Runtime::FlushBarriers
    KRenderPassSwitch,///< Scheduler::BeginRendering / EndRendering
    KEncPipeline,    ///< recorder: vkCmdBindPipeline
    KEncDescriptors, ///< recorder: push constants, push descriptors, descriptor set binds
    KEncDraw,        ///< recorder: draws (direct, indirect) and index buffer binds
    KEncDispatch,    ///< recorder: dispatches
    KEncRendering,   ///< recorder: vkCmdBeginRendering / vkCmdEndRendering
    KEncBarrier,     ///< recorder: pipeline barriers from Runtime::FlushBarriers
    KEncCopy,        ///< recorder: copies, fills, clears (Runtime)
    KDeferredWriteData, ///< deferred-signal waits before an in-order WRITE_DATA (RunWriteData)
    KDeferredFlip,      ///< deferred-signal waits before a flip's interrupt (SignalFlip)
    KCount
};
inline constexpr const char* k_names[KCount] = {
    "stage B drain at DUMP_CONST_RAM", "stage B drain at idle", "stage B drain at submit/flush",
    "stage B drain at other packets", "draw pipe full", "deferred signals (GPU completion)",
    "recorder signals", "recorder sync", "host copies", "GPU ticks", "presentation label",
    "queue submit", "image refresh", "texel sync (image tiled to memory)", "pipeline build wait", "descriptor build",
    "dynamic state", "barrier flush", "render pass switch", "encode pipeline bind",
    "encode descriptors", "encode draws", "encode dispatches", "encode rendering begin/end",
    "encode barriers", "encode copies", "deferred signals before WRITE_DATA",
    "deferred signals before a flip"};

/// The scope kind WaitDeferredSignals charges on this thread (KDeferred unless a caller names
/// its site).
inline thread_local K deferred_site = KDeferred;

/// Names the caller of the WaitDeferredSignals calls in its lifetime.
class DeferredSite {
public:
    explicit DeferredSite(K site) : previous{deferred_site} {
        deferred_site = site;
    }
    ~DeferredSite() {
        deferred_site = previous;
    }
    DeferredSite(const DeferredSite&) = delete;
    DeferredSite& operator=(const DeferredSite&) = delete;

private:
    K previous;
};

/// Why the draw scheduler submitted (Flush census, docs/macos/parse-split.md).
enum F : std::uint32_t {
    FFrameEnd,   ///< the frame end (Liverpool, after OnSubmit)
    FLabel,      ///< a label not batched (stage A, or recording not deferred): one per label
    FBatchIdle,  ///< stage B's batched labels, at a packet boundary with the draw pipe idle
    FBatchAge,   ///< stage B's batched labels, the oldest older than LabelBatchAge
    FBatchSleep, ///< stage B's batched labels, before it sleeps (bit 56)
    FBatchAsked, ///< stage B's batched labels, asked for by a waiting thread (bit 56)
    FBatchForWait, ///< stage B's batched labels, at a WAIT_REG_MEM stage A met early
    FFinish,     ///< Scheduler::Finish (a wait for the GPU)
    FOther,
    FCount
};
inline constexpr const char* f_names[FCount] = {
    "frame end", "unbatched labels", "label batch at idle", "label batch at age",
    "label batch before sleep", "label batch asked for", "label batch at a WAIT met early",
    "Finish",
    "other"};

/// The reason the calling thread's next draw-scheduler Flush has (FOther unless set).
inline thread_local F flush_reason = FOther;

/// Sets this thread's flush reason for the life of the object.
class FlushReason {
public:
    explicit FlushReason(F reason) : previous{flush_reason} {
        flush_reason = reason;
    }
    ~FlushReason() {
        flush_reason = previous;
    }
    FlushReason(const FlushReason&) = delete;
    FlushReason& operator=(const FlushReason&) = delete;

private:
    F previous;
};

/// Submissions of the draw scheduler, any thread; read at reports.
struct FlushCensus {
    std::array<std::atomic<std::uint64_t>, FCount> by_reason{};
    /// Labels at GPU completion written, their issue-to-write age (the label thread), and the
    /// window's maximum (reset at each report).
    std::atomic<std::uint64_t> labels_written{0}, label_age_ns{0}, label_age_window_max_ns{0};
    /// WAIT_REG_MEMs stage A met early from a pending value (their in-order markers ran).
    std::atomic<std::uint64_t> forwarded_waits{0};
    std::atomic<std::uint64_t> ended_pass{0};       ///< a render pass was open (it ends)
    std::atomic<std::uint64_t> batches{0}, batch_age_ns{0}, batch_age_max_ns{0};
};
inline FlushCensus flush_census;

/// A draw-scheduler submission, with a render pass open or not.
inline void NoteFlush(bool pass_open) {
    flush_census.by_reason[flush_reason].fetch_add(1, std::memory_order_relaxed);
    if (pass_open) {
        flush_census.ended_pass.fetch_add(1, std::memory_order_relaxed);
    }
}

/// A label at GPU completion written `age_ns` after it was issued (the guest sees it then).
inline void NoteLabelWritten(std::uint64_t age_ns) {
    flush_census.labels_written.fetch_add(1, std::memory_order_relaxed);
    flush_census.label_age_ns.fetch_add(age_ns, std::memory_order_relaxed);
    std::uint64_t max = flush_census.label_age_window_max_ns.load(std::memory_order_relaxed);
    while (age_ns > max && !flush_census.label_age_window_max_ns.compare_exchange_weak(
                               max, age_ns, std::memory_order_relaxed)) {
    }
}

/// A batch of stage B labels submitted after waiting `age_ns` (its oldest label).
inline void NoteLabelBatch(std::uint64_t age_ns) {
    flush_census.batches.fetch_add(1, std::memory_order_relaxed);
    flush_census.batch_age_ns.fetch_add(age_ns, std::memory_order_relaxed);
    std::uint64_t max = flush_census.batch_age_max_ns.load(std::memory_order_relaxed);
    while (age_ns > max && !flush_census.batch_age_max_ns.compare_exchange_weak(
                               max, age_ns, std::memory_order_relaxed)) {
    }
}

constexpr std::uint32_t MaxBuckets = 16;
constexpr std::uint32_t MaxDepth = 8; ///< nested scopes

struct Account {
    std::array<std::atomic<std::uint64_t>, MaxBuckets> bucket{};
    std::array<std::atomic<std::uint64_t>, KCount> scope{};
    /// Scope time by the bucket open while it ran (e.g. deferred-signal waits by packet class).
    std::array<std::atomic<std::uint64_t>, KCount * MaxBuckets> scope_in{};
    // Writer only: the last transition, the open bucket and the open scopes (innermost last).
    std::uint64_t last = 0;
    std::uint32_t open = 0, depth = 0;
    std::array<K, MaxDepth> stack{};
    /// The thread's Mach port (Attach), for its on-CPU time at reports.
    std::atomic<mach_port_t> thread{MACH_PORT_NULL};
};

inline Account stage_a, stage_b, recorder;
inline thread_local Account* current = nullptr;

inline void Add(std::atomic<std::uint64_t>& counter, std::uint64_t value) {
    counter.store(counter.load(std::memory_order_relaxed) + value, std::memory_order_relaxed);
}

[[noreturn]] inline void Stop(const char* why) {
    std::fprintf(stderr, "STOP: parse split: %s\n", why);
    std::abort();
}

/// Charges the time since the last transition to whatever owned it: the innermost open scope
/// (and, per bucket, the bucket open meanwhile), else the open bucket. Every transition calls
/// this first, so a thread's time is partitioned exactly.
inline void Charge(Account& a, std::uint64_t now) {
    const std::uint64_t span = now - a.last;
    if (a.depth) {
        const K kind = a.stack[a.depth - 1];
        Add(a.scope[kind], span);
        Add(a.scope_in[kind * MaxBuckets + a.open], span);
    } else {
        Add(a.bucket[a.open], span);
    }
    a.last = now;
}

/// Starts accounting this thread in `account`, in `bucket`.
inline void Attach(Account& account, std::uint32_t bucket) {
    current = &account;
    account.thread.store(pthread_mach_thread_np(pthread_self()), std::memory_order_relaxed);
    account.last = __rdtsc();
    account.open = bucket;
    account.depth = 0;
}

/// Opens `bucket` from now (a no-op when it is already open: charging can wait).
inline void Mark(std::uint32_t bucket) {
    Account* const a = current;
    if (!a || a->open == bucket) {
        return;
    }
    Charge(*a, __rdtsc());
    a->open = bucket;
}

/// Charges this thread's time up to now (before a report).
inline void Flush() {
    if (Account* const a = current) {
        Charge(*a, __rdtsc());
    }
}

/// The bucket currently open on this thread.
inline std::uint32_t Open() {
    return current ? current->open : 0;
}

/// Before a coroutine yields: its open bucket, for Resume. A scope must not span a yield (other
/// tasks run meanwhile): that stops.
inline std::uint32_t Suspend() {
    if (current && current->depth) {
        Stop("a scope is open across a coroutine yield");
    }
    return Open();
}

/// After the coroutine resumes: reopens its bucket (the time it was suspended stays with
/// whatever ran meanwhile).
inline void Resume(std::uint32_t bucket) {
    Mark(bucket);
}

/// Marks `bucket` only on `account`'s thread (code shared by stage A and stage B).
inline void MarkOn(const Account& account, std::uint32_t bucket) {
    if (current == &account) {
        Mark(bucket);
    }
}

/// A wait or named piece of work. While open it owns its thread's time (inner scopes take their
/// own); marks inside it change only the bucket its time is listed under.
class Scope {
public:
    explicit Scope(K kind) : account{current} {
        if (!account) {
            return;
        }
        if (account->depth == MaxDepth) {
            Stop("scopes nested too deeply");
        }
        Charge(*account, __rdtsc());
        account->stack[account->depth++] = kind;
    }
    ~Scope() {
        if (!account) {
            return;
        }
        Charge(*account, __rdtsc());
        --account->depth;
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    Account* account;
};

/// On `account`'s thread: marks `bucket` for the life of the object, then reopens the bucket
/// open before.
class Section {
public:
    Section(const Account& account, std::uint32_t bucket)
        : active{current == &account}, previous{Open()} {
        if (active) {
            Mark(bucket);
        }
    }
    ~Section() {
        if (active) {
            Mark(previous);
        }
    }
    Section(const Section&) = delete;
    Section& operator=(const Section&) = delete;

private:
    bool active;
    std::uint32_t previous;
};

namespace detail {
struct Previous {
    std::array<std::uint64_t, MaxBuckets> bucket{};
    std::array<std::uint64_t, KCount> scope{};
    std::array<std::uint64_t, KCount * MaxBuckets> scope_in{};
};

/// Appends "name value" for each nonzero entry over `per_frame_ms` and updates `previous`.
inline std::string Line(const Account& a, Previous& previous, const char* const* names,
                        std::uint32_t count, double ms_per_cycle, double frames,
                        std::uint64_t& total_cycles) {
    std::string out;
    char item[96];
    total_cycles = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint64_t now = a.bucket[i].load(std::memory_order_relaxed);
        const std::uint64_t delta = now - previous.bucket[i];
        previous.bucket[i] = now;
        total_cycles += delta;
        if (delta * ms_per_cycle / frames >= 0.005) {
            std::snprintf(item, sizeof(item), "%s%s %.2f", out.empty() ? "" : ", ", names[i],
                          delta * ms_per_cycle / frames);
            out += item;
        }
    }
    std::string waits;
    std::uint64_t scoped = 0;
    for (std::uint32_t k = 0; k < KCount; ++k) {
        const std::uint64_t now = a.scope[k].load(std::memory_order_relaxed);
        const std::uint64_t delta = now - previous.scope[k];
        previous.scope[k] = now;
        scoped += delta;
        if (delta * ms_per_cycle / frames >= 0.005) {
            std::snprintf(item, sizeof(item), "%s%s %.2f", waits.empty() ? "" : ", ", k_names[k],
                          delta * ms_per_cycle / frames);
            waits += item;
        }
    }
    total_cycles += scoped;
    return out + "; in scopes: " + (waits.empty() ? "none" : waits);
}

/// "kind during: bucket value, ..." for one scope kind, by the bucket open while it ran.
inline std::string ScopeBy(const Account& a, Previous& previous, K kind, const char* const* names,
                           std::uint32_t count, double ms_per_cycle, double frames) {
    std::string out;
    char item[96];
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t slot = kind * MaxBuckets + i;
        const std::uint64_t now = a.scope_in[slot].load(std::memory_order_relaxed);
        const std::uint64_t delta = now - previous.scope_in[slot];
        previous.scope_in[slot] = now;
        if (delta * ms_per_cycle / frames >= 0.005) {
            std::snprintf(item, sizeof(item), "%s%s %.2f", out.empty() ? "" : ", ", names[i],
                          delta * ms_per_cycle / frames);
            out += item;
        }
    }
    return out.empty() ? "none" : out;
}

inline Previous previous_a, previous_b, previous_r;
inline std::array<std::uint64_t, FCount> previous_flushes{};
inline std::uint64_t previous_ended_pass = 0, previous_batches = 0, previous_batch_age_ns = 0,
                     previous_labels_written = 0, previous_label_age_ns = 0,
                     previous_forwarded_waits = 0;

/// "  Flushes …" for the window: per frame by reason, passes ended, label batch age.
inline std::string FlushLine(double frames) {
    std::string out;
    char item[96];
    std::uint64_t total = 0;
    for (std::uint32_t r = 0; r < FCount; ++r) {
        const std::uint64_t now = flush_census.by_reason[r].load(std::memory_order_relaxed);
        const std::uint64_t delta = now - previous_flushes[r];
        previous_flushes[r] = now;
        total += delta;
        std::snprintf(item, sizeof(item), "%s%s %.1f", r ? ", " : "", f_names[r],
                      frames ? delta / frames : 0.0);
        out += item;
    }
    const std::uint64_t ended = flush_census.ended_pass.load(std::memory_order_relaxed);
    const std::uint64_t batches = flush_census.batches.load(std::memory_order_relaxed);
    const std::uint64_t age = flush_census.batch_age_ns.load(std::memory_order_relaxed);
    const std::uint64_t window_batches = batches - previous_batches;
    char tail[256];
    std::snprintf(tail, sizeof(tail),
                  "; %.1f per frame ended an open render pass; label batches %.1f per frame, "
                  "oldest label's age at submit %.0f us mean, %.0f us max (all so far)",
                  frames ? (ended - previous_ended_pass) / frames : 0.0,
                  frames ? window_batches / frames : 0.0,
                  window_batches ? (age - previous_batch_age_ns) / 1e3 / window_batches : 0.0,
                  flush_census.batch_age_max_ns.load(std::memory_order_relaxed) / 1e3);
    previous_ended_pass = ended;
    previous_batches = batches;
    previous_batch_age_ns = age;
    const std::uint64_t written = flush_census.labels_written.load(std::memory_order_relaxed);
    const std::uint64_t written_age = flush_census.label_age_ns.load(std::memory_order_relaxed);
    const std::uint64_t window_written = written - previous_labels_written;
    const std::uint64_t window_max =
        flush_census.label_age_window_max_ns.exchange(0, std::memory_order_relaxed);
    const std::uint64_t forwarded = flush_census.forwarded_waits.load(std::memory_order_relaxed);
    const double forwarded_per_frame = frames ? (forwarded - previous_forwarded_waits) / frames : 0.0;
    previous_forwarded_waits = forwarded;
    char written_text[256];
    std::snprintf(written_text, sizeof(written_text),
                  "; WAIT_REG_MEMs met early %.1f per frame; labels written %.1f per frame, issue "
                  "to write %.0f us mean, %.0f us max",
                  forwarded_per_frame, frames ? window_written / frames : 0.0,
                  window_written ? (written_age - previous_label_age_ns) / 1e3 / window_written
                                 : 0.0,
                  window_max / 1e3);
    previous_labels_written = written;
    previous_label_age_ns = written_age;
    char head[96];
    std::snprintf(head, sizeof(head), "  Flushes of the draw scheduler: %.1f per frame = ",
                  frames ? total / frames : 0.0);
    return head + out + tail + written_text;
}
inline std::uint64_t window_cycles = 0, window_frames = 0, window_draws = 0;
/// The account's thread's user + system CPU time in ns (thread_info), or ~0 when unavailable
/// (no thread attached yet, or the call failed).
inline std::uint64_t CpuNs(const Account& a) {
    const mach_port_t thread = a.thread.load(std::memory_order_relaxed);
    if (thread == MACH_PORT_NULL) {
        return ~0ull;
    }
    thread_basic_info_data_t info{};
    mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
    if (thread_info(thread, THREAD_BASIC_INFO, reinterpret_cast<thread_info_t>(&info), &count) !=
        KERN_SUCCESS) {
        return ~0ull;
    }
    const auto ns = [](const time_value_t& t) {
        return std::uint64_t(t.seconds) * 1000000000ull + std::uint64_t(t.microseconds) * 1000ull;
    };
    return ns(info.user_time) + ns(info.system_time);
}
/// On-CPU ms per frame since the previous call ("n/a" when either reading is unavailable): wall
/// time a thread spends neither blocked nor waiting for a core.
inline std::string OnCpu(const Account& a, std::uint64_t& previous, double frames) {
    const std::uint64_t now = CpuNs(a);
    const bool known = now != ~0ull && previous != ~0ull && now >= previous && frames > 0;
    char text[48];
    if (known) {
        std::snprintf(text, sizeof(text), "on CPU %.2f ms", double(now - previous) / 1e6 / frames);
    } else {
        std::snprintf(text, sizeof(text), "on CPU n/a");
    }
    previous = now;
    return text;
}
inline std::uint64_t previous_cpu_a = ~0ull, previous_cpu_b = ~0ull, previous_cpu_r = ~0ull;
inline std::chrono::steady_clock::time_point window_start{};
} // namespace detail

/// On stage A at each frame end: every 600, prints the split per frame and starts a new window.
/// `draws`: the running draw count (for draws per frame).
inline void NoteFrameEnd(std::uint64_t draws) {
    using namespace detail;
    const std::uint64_t now_cycles = __rdtsc();
    const auto now = std::chrono::steady_clock::now();
    if (window_cycles == 0) {
        Flush();
        window_cycles = now_cycles;
        window_start = now;
        window_draws = draws;
        std::uint64_t ignored = 0; // baselines
        Line(stage_a, previous_a, a_names, ACount, 0, 1, ignored);
        Line(stage_b, previous_b, b_names, BCount, 0, 1, ignored);
        Line(recorder, previous_r, r_names, RCount, 0, 1, ignored);
        FlushLine(0);
        OnCpu(stage_a, previous_cpu_a, 0);
        OnCpu(stage_b, previous_cpu_b, 0);
        OnCpu(recorder, previous_cpu_r, 0);
        for (std::uint32_t k = 0; k < KCount; ++k) {
            ScopeBy(stage_a, previous_a, static_cast<K>(k), a_names, ACount, 0, 1);
            ScopeBy(stage_b, previous_b, static_cast<K>(k), b_names, BCount, 0, 1);
        }
        window_frames = 0;
        return;
    }
    if (++window_frames < 600) {
        return;
    }
    Flush(); // stage A: its open interval belongs to this window
    const double wall_ms =
        std::chrono::duration<double, std::milli>(now - window_start).count();
    const double ms_per_cycle = wall_ms / double(now_cycles - window_cycles);
    const double frames = double(window_frames);
    // Stage A's direct-draw time per draw (pipeline selection, packet build), before Line moves
    // the baseline.
    const std::uint64_t window_draw_count = draws - window_draws;
    const double a_us_per_draw =
        window_draw_count ? double(stage_a.bucket[ADraw].load(std::memory_order_relaxed) -
                                   previous_a.bucket[ADraw]) *
                                ms_per_cycle * 1000.0 / double(window_draw_count)
                          : 0.0;
    std::uint64_t a_cycles = 0, b_cycles = 0, r_cycles = 0;
    const std::string a_line =
        Line(stage_a, previous_a, a_names, ACount, ms_per_cycle, frames, a_cycles);
    const std::string a_deferred =
        ScopeBy(stage_a, previous_a, KDeferred, a_names, ACount, ms_per_cycle, frames);
    const std::string a_drain =
        ScopeBy(stage_a, previous_a, KDrainPacket, a_names, ACount, ms_per_cycle, frames);
    for (std::uint32_t k = 0; k < KCount; ++k) {
        if (k != KDeferred && k != KDrainPacket) {
            ScopeBy(stage_a, previous_a, static_cast<K>(k), a_names, ACount, 0, 1);
        }
    }
    const std::string b_line =
        Line(stage_b, previous_b, b_names, BCount, ms_per_cycle, frames, b_cycles);
    for (std::uint32_t k = 0; k < KCount; ++k) {
        ScopeBy(stage_b, previous_b, static_cast<K>(k), b_names, BCount, 0, 1);
    }
    const std::string r_line =
        Line(recorder, previous_r, r_names, RCount, ms_per_cycle, frames, r_cycles);
    const double frame_ms = wall_ms / frames;
    const std::string flush_line = FlushLine(frames);
    const std::string a_cpu = OnCpu(stage_a, previous_cpu_a, frames);
    const std::string b_cpu = OnCpu(stage_b, previous_cpu_b, frames);
    const std::string r_cpu = OnCpu(recorder, previous_cpu_r, frames);
    // Printed like the Stall lines: Info-level logging is filtered in perf runs.
    std::printf("Parse split, last 600 frame ends: %.2f ms per frame, %.0f draws per frame "
                "(stage A %.2f us per draw); ms per frame by thread (accounted / wall)\n"
                "  stage A (GPU command thread, %.0f%%, %s per frame): %s\n"
                "    deferred-signal waits during: %s\n"
                "    stage B drains at other packets during: %s\n"
                "  stage B (draw recording thread, %.0f%%, %s per frame): %s\n"
                "  Vulkan recording thread (%.0f%%, %s per frame): %s\n"
                "%s\n",
                frame_ms, double(window_draw_count) / frames, a_us_per_draw,
                100.0 * a_cycles * ms_per_cycle / wall_ms, a_cpu.c_str(), a_line.c_str(),
                a_deferred.c_str(), a_drain.c_str(), 100.0 * b_cycles * ms_per_cycle / wall_ms,
                b_cpu.c_str(), b_line.c_str(), 100.0 * r_cycles * ms_per_cycle / wall_ms,
                r_cpu.c_str(), r_line.c_str(), flush_line.c_str());
    std::fflush(stdout);
    window_cycles = now_cycles;
    window_start = now;
    window_draws = draws;
    window_frames = 0;
}

} // namespace BbSplit

#define BB_SPLIT_CAT2(a, b) a##b
#define BB_SPLIT_CAT(a, b) BB_SPLIT_CAT2(a, b)
#define BB_SPLIT_SCOPE(kind) ::BbSplit::Scope BB_SPLIT_CAT(bb_split_scope_, __LINE__){::BbSplit::kind}
#define BB_SPLIT_SECTION(account, bucket)                                                          \
    ::BbSplit::Section BB_SPLIT_CAT(bb_split_section_, __LINE__){::BbSplit::account,               \
                                                                 ::BbSplit::bucket}
#define BB_SPLIT_MARK_ON(account, bucket) ::BbSplit::MarkOn(::BbSplit::account, ::BbSplit::bucket)
#else
#define BB_SPLIT_SCOPE(kind) ((void)0)
#define BB_SPLIT_SECTION(account, bucket) ((void)0)
#define BB_SPLIT_MARK_ON(account, bucket) ((void)0)
#endif
