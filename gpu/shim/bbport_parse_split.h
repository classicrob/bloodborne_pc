// bbport (macOS): where the GPU-side threads' time goes. "Parse split" lines, every 600 frame
// ends (docs/macos/parse-split.md).
//
// Each instrumented thread's time is partitioned. Mark(bucket) closes the interval since the
// previous mark and charges it to the bucket that mark named, less the scopes (Scope) inside it,
// which are charged to their own kinds. Scopes nest; each is charged its own time without its
// inner scopes. A thread with no account (current == nullptr) records nothing.
//
// One writer per account (its thread); the reporter reads with relaxed loads. Always on: a mark
// or scope costs two counter reads and a few stores.
#pragma once
#ifdef __APPLE__
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
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
    KCount
};
inline constexpr const char* k_names[KCount] = {
    "stage B drain at DUMP_CONST_RAM", "stage B drain at idle", "stage B drain at submit/flush",
    "stage B drain at other packets", "draw pipe full", "deferred signals (GPU completion)",
    "recorder signals", "recorder sync", "host copies", "GPU ticks", "presentation label",
    "queue submit"};

constexpr std::uint32_t MaxBuckets = 16;

struct Account {
    std::array<std::atomic<std::uint64_t>, MaxBuckets> bucket{};
    std::array<std::atomic<std::uint64_t>, KCount> scope{};
    /// Scope time by the bucket open when it ran (e.g. deferred-signal waits by packet class).
    std::array<std::atomic<std::uint64_t>, KCount * MaxBuckets> scope_in{};
    // Writer only:
    std::uint64_t mark_at = 0, scoped_total = 0, scoped_at_mark = 0;
    std::uint32_t open = 0;
};

inline Account stage_a, stage_b, recorder;
inline thread_local Account* current = nullptr;

inline void Add(std::atomic<std::uint64_t>& counter, std::uint64_t value) {
    counter.store(counter.load(std::memory_order_relaxed) + value, std::memory_order_relaxed);
}

/// Starts accounting this thread in `account`, in `bucket`.
inline void Attach(Account& account, std::uint32_t bucket) {
    current = &account;
    account.mark_at = __rdtsc();
    account.scoped_at_mark = account.scoped_total;
    account.open = bucket;
}

/// Charges the time since the previous mark to its bucket; `bucket` is open from now.
inline void Mark(std::uint32_t bucket) {
    Account* const a = current;
    if (!a) {
        return;
    }
    const std::uint64_t now = __rdtsc();
    const std::uint64_t span = now - a->mark_at;
    const std::uint64_t scoped = a->scoped_total - a->scoped_at_mark;
    Add(a->bucket[a->open], span > scoped ? span - scoped : 0);
    a->mark_at = now;
    a->scoped_at_mark = a->scoped_total;
    a->open = bucket;
}

/// The bucket currently open on this thread (for restoring it after a nested mark).
inline std::uint32_t Open() {
    return current ? current->open : 0;
}

/// A wait or named piece of work; charged its own time, without inner scopes.
class Scope {
public:
    explicit Scope(K kind) : account{current}, kind{kind} {
        if (account) {
            inner_before = account->scoped_total;
            start = __rdtsc();
        }
    }
    ~Scope() {
        if (!account) {
            return;
        }
        const std::uint64_t span = __rdtsc() - start;
        const std::uint64_t inner = account->scoped_total - inner_before;
        const std::uint64_t own = span > inner ? span - inner : 0;
        Add(account->scope[kind], own);
        Add(account->scope_in[kind * MaxBuckets + account->open], own);
        account->scoped_total += own;
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    Account* account;
    K kind;
    std::uint64_t start = 0, inner_before = 0;
};

/// Marks `bucket` only on `account`'s thread (code shared by stage A and stage B).
inline void MarkOn(const Account& account, std::uint32_t bucket) {
    if (current == &account) {
        Mark(bucket);
    }
}

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
inline std::uint64_t window_cycles = 0, window_frames = 0, window_draws = 0;
inline std::chrono::steady_clock::time_point window_start{};
} // namespace detail

/// On stage A at each frame end: every 600, prints the split per frame and starts a new window.
/// `draws`: the running draw count (for draws per frame).
inline void NoteFrameEnd(std::uint64_t draws) {
    using namespace detail;
    const std::uint64_t now_cycles = __rdtsc();
    const auto now = std::chrono::steady_clock::now();
    if (window_cycles == 0) {
        window_cycles = now_cycles;
        window_start = now;
        window_draws = draws;
        std::uint64_t ignored = 0; // baselines
        Line(stage_a, previous_a, a_names, ACount, 0, 1, ignored);
        Line(stage_b, previous_b, b_names, BCount, 0, 1, ignored);
        Line(recorder, previous_r, r_names, RCount, 0, 1, ignored);
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
    const double wall_ms =
        std::chrono::duration<double, std::milli>(now - window_start).count();
    const double ms_per_cycle = wall_ms / double(now_cycles - window_cycles);
    const double frames = double(window_frames);
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
    // Printed like the Stall lines: Info-level logging is filtered in perf runs.
    std::printf("Parse split, last 600 frame ends: %.2f ms per frame, %.0f draws per frame; "
                "ms per frame by thread (accounted / wall)\n"
                "  stage A (GPU command thread, %.0f%%): %s\n"
                "    deferred-signal waits during: %s\n"
                "    stage B drains at other packets during: %s\n"
                "  stage B (draw recording thread, %.0f%%): %s\n"
                "  Vulkan recording thread (%.0f%%): %s\n",
                frame_ms, double(draws - window_draws) / frames,
                100.0 * a_cycles * ms_per_cycle / wall_ms, a_line.c_str(), a_deferred.c_str(),
                a_drain.c_str(), 100.0 * b_cycles * ms_per_cycle / wall_ms, b_line.c_str(),
                100.0 * r_cycles * ms_per_cycle / wall_ms, r_line.c_str());
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
