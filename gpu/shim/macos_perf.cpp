// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef __APPLE__
#include "macos_perf.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "bbport_toggles.h"
#include "../../src/compat_mac.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace BbPerf {
namespace {
using Clock = std::chrono::steady_clock;
[[noreturn]] void Fail(const char* message) {
    std::fprintf(stderr, "macOS performance measurement: %s\n", message);
    std::abort();
}
void Check(VkResult result, const char* call) {
    if (result == VK_SUCCESS) return;
    std::fprintf(stderr, "macOS performance measurement: %s returned VkResult %d\n", call, int(result));
    std::abort();
}
uint64_t ThreadCpu() {
    timespec time{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time)) Fail("thread CPU clock failed");
    return uint64_t(time.tv_sec) * 1000000000 + time.tv_nsec;
}
// 0/unset: off; 1: on; anything else is a configuration error.
bool Flag(const char* name) {
    const char* value = std::getenv(name);
    if (!value || !*value || !std::strcmp(value, "0")) return false;
    if (std::strcmp(value, "1")) {
        std::fprintf(stderr, "macOS performance measurement: %s must be 0 or 1\n", name);
        std::abort();
    }
    return true;
}
std::atomic<uint64_t> recorder_cpu{0}, recorder_chunks{0}, import_ns{0}, import_count{0};
struct Counters {
    uint64_t liverpool = 0, recorder = 0, chunks = 0, idle = 0, fences = 0,
             deferred_signals = 0, copy_record = 0, copy_workers = 0, record_wait = 0, import = 0, imports = 0;
};
Counters ReadCounters() {
    Counters c;
    if (const auto port = BbStats::gpu_thread_port.load(std::memory_order_relaxed)) {
        int64_t user = 0, system = 0;
        if (compat_thread_cpu_times(port, &user, &system)) Fail("Liverpool Mach CPU query failed");
        c.liverpool = uint64_t(user + system) * 1000;
    }
    c.recorder = recorder_cpu.load(std::memory_order_relaxed);
    c.chunks = recorder_chunks.load(std::memory_order_relaxed);
    c.idle = BbStats::gpu_idle_ns.load(std::memory_order_relaxed);
    c.fences = BbStats::tick_wait_ns.load(std::memory_order_relaxed);
    c.deferred_signals = BbStats::deferred_signals_wait_ns.load(std::memory_order_relaxed);
    c.copy_record = BbStats::host_copy_record_wait_ns.load(std::memory_order_relaxed);
    c.copy_workers = BbStats::copy_threads_wait_ns.load(std::memory_order_relaxed);
    c.record_wait = BbStats::sync_recording_ns.load(std::memory_order_relaxed);
    c.import = import_ns.load(std::memory_order_relaxed);
    c.imports = import_count.load(std::memory_order_relaxed);
    return c;
}
constexpr const char* PhaseNames[NumPhases] = {"scene", "post", "upscaler", "present"};

struct Reporter {
    std::mutex mutex;
    FILE* file;
    const bool phases = Phases();
    Clock::time_point start = Clock::now(), window = start, last_flip{};
    Counters previous{};
    std::map<std::string, std::vector<double>> samples;
    uint64_t flips = 0;
    // GPU execution of completed submissions in this window, both streams (same queue).
    std::vector<std::pair<double, double>> intervals; ///< [begin, end] in ms on the GPU clock
    std::array<double, 2> stream_ms{};                ///< main, present: sum of submissions
    std::array<uint64_t, 2> stream_submissions{};
    uint64_t deferred_reads = 0; ///< timestamp reads retried: timeline done, results not yet
    Reporter() {
        const char* path = std::getenv("BB_PERF_OUT");
        if (!path || !*path) Fail("BB_PERF=1 requires BB_PERF_OUT (a new JSONL file)");
        file = std::fopen(path, "wx");
        if (!file) Fail("cannot create BB_PERF_OUT (it must not already exist)");
        std::fprintf(file, "{\"type\":\"schema\",\"version\":3,\"window_seconds\":5,\"perturbed\":%s,"
            "\"gpu_source\":\"first and last timestamp of each submission (tests/macos_perf/timestamps.sh)\","
            "\"gpu_busy_union\":\"union of submission spans on the GPU clock, both streams\","
            "\"phases\":\"%s\"}\n", phases ? "true" : "false",
            phases ? "per-frame phase times from one submission per phase (BB_PERF_PHASES=1)"
                   : "not measured (BB_PERF_PHASES=0)");
        Flush();
    }
    void Flush() {
        if (std::fflush(file) || std::ferror(file)) Fail("performance JSONL write failed");
    }
    void Add(const char* name, double value) { samples[name].push_back(value); }
    double BusyUnion() {
        std::sort(intervals.begin(), intervals.end());
        double busy = 0, begin = 0, end = -1;
        for (const auto& [b, e] : intervals) {
            if (b > end) {
                if (end > begin) busy += end - begin;
                begin = b; end = e;
            } else {
                end = std::max(end, e);
            }
        }
        if (end > begin) busy += end - begin;
        return busy;
    }
    // Every 5 s; `final` also writes the partial last window (orderly renderer teardown).
    void Print(Clock::time_point now, bool final = false) {
        if (!final && now - window < std::chrono::seconds(5)) return;
        if (final && !flips && intervals.empty()) return;
        const double elapsed = std::chrono::duration<double>(now - start).count();
        const double seconds = std::chrono::duration<double>(now - window).count();
        const double busy = BusyUnion();
        std::fprintf(file, "{\"type\":\"window\",\"final\":%s,\"perturbed\":%s,\"elapsed_seconds\":%.6f,"
            "\"seconds\":%.6f,\"flips\":%llu,\"gpu\":{\"main_submissions\":%llu,\"present_submissions\":%llu,"
            "\"deferred_reads\":%llu,\"main_ms\":%.6f,\"present_ms\":%.6f,\"busy_union_ms\":%.6f,"
            "\"busy_union_ms_per_flip\":",
            final ? "true" : "false", phases ? "true" : "false", elapsed, seconds,
            (unsigned long long)flips, (unsigned long long)stream_submissions[0],
            (unsigned long long)stream_submissions[1], (unsigned long long)deferred_reads,
            stream_ms[0], stream_ms[1], busy);
        if (flips) std::fprintf(file, "%.6f", busy / double(flips));
        else std::fprintf(file, "null");
        std::fprintf(file, "},\"metrics\":{");
        bool comma = false;
        for (auto& [name, values] : samples) {
            if (values.empty()) continue;
            std::sort(values.begin(), values.end());
            double sum = 0;
            for (double value : values) sum += value;
            const auto percentile = [&](double p) {
                return values[std::min(values.size() - 1, size_t(std::ceil(p * values.size())) - 1)];
            };
            std::fprintf(file, "%s\"%s\":{\"n\":%zu,\"mean\":%.6f,\"p50\":%.6f,\"p95\":%.6f,\"p99\":%.6f,\"max\":%.6f}",
                comma ? "," : "", name.c_str(), values.size(), sum / values.size(),
                percentile(.5), percentile(.95), percentile(.99), values.back());
            comma = true;
        }
        std::fprintf(file, "}}\n");
        Flush();
        samples.clear(); intervals.clear(); stream_ms = {}; stream_submissions = {}; flips = 0;
        deferred_reads = 0;
        window = now;
    }
};
Reporter& Report() { static Reporter report; return report; }
} // namespace

bool Enabled() {
    static const bool enabled = [] {
        if (!Flag("BB_PERF")) {
            if (Flag("BB_PERF_PHASES")) Fail("BB_PERF_PHASES=1 requires BB_PERF=1");
            return false;
        }
        if (Flag("BB_GPU_PROFILE")) Fail("BB_PERF and BB_GPU_PROFILE cannot run together");
        (void)Report();
        return true;
    }();
    return enabled;
}

bool Phases() {
    static const bool phases = [] {
        if (!Flag("BB_PERF_PHASES")) return false;
        if (!Flag("BB_PERF")) Fail("BB_PERF_PHASES=1 requires BB_PERF=1");
        return true;
    }();
    return phases;
}

struct GpuStream::Impl {
    static constexpr unsigned Slots = 512; // submissions in flight; 2 timestamps each
    struct Slot {
        bool used = false;
        uint64_t tick = 0, frame = 0;
        Phase phase = Phase::Scene;
        Clock::time_point completed_at{}; ///< first seen complete but its results not ready
    };
    VkDevice device;
    VkQueryPool pool;
    VkSemaphore completion;
    bool present;
    unsigned active = 0, next = 0;
    uint64_t mask, last_tick = 0, collected_frame = 0;
    double period;
    std::array<double, NumPhases> frame_ms{};
    std::array<Slot, Slots> slots{};
    std::deque<unsigned> pending;

    void FinishFrame(Reporter& report) {
        double total = 0;
        for (unsigned p = 0; p < NumPhases; ++p) {
            if (Phase(p) == Phase::Present) continue;
            report.Add((std::string("gpu_") + PhaseNames[p] + "_ms").c_str(), frame_ms[p]);
            total += frame_ms[p];
        }
        report.Add("gpu_frame_work_ms", total);
        frame_ms = {};
    }
    void Collect() {
        uint64_t complete = 0;
        Check(VULKAN_HPP_DEFAULT_DISPATCHER.vkGetSemaphoreCounterValue(device, completion, &complete),
              "vkGetSemaphoreCounterValue");
        while (!pending.empty() && slots[pending.front()].tick <= complete) {
            const unsigned index = pending.front();
            auto& slot = slots[index];
            struct Result { uint64_t timestamp, available; };
            std::array<Result, 2> stamps{};
            const VkResult result = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetQueryPoolResults(device, pool,
                index * 2, 2, sizeof(stamps), stamps.data(), sizeof(Result),
                VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
            // MoltenVK signals the timeline before it has resolved the submission's timestamps
            // (docs/macos/perf.md): VK_NOT_READY here means "later", so the slot stays pending
            // and is read at a later submission. Still unavailable 2 s after completion is an error.
            if (result == VK_NOT_READY) {
                const auto now = Clock::now();
                if (slot.completed_at == Clock::time_point{}) slot.completed_at = now;
                else if (now - slot.completed_at > std::chrono::seconds(2))
                    Fail("vkGetQueryPoolResults still VK_NOT_READY 2 s after the submission completed");
                auto& report = Report();
                std::scoped_lock lock{report.mutex};
                ++report.deferred_reads;
                return;
            }
            Check(result, "vkGetQueryPoolResults");
            if (!stamps[0].available || !stamps[1].available)
                Fail("vkGetQueryPoolResults returned VK_SUCCESS with an unavailable timestamp");
            const double begin = double(stamps[0].timestamp & mask) * period / 1e6;
            const double ms = double((stamps[1].timestamp - stamps[0].timestamp) & mask) * period / 1e6;
            if (!std::isfinite(ms) || !std::isfinite(begin)) Fail("invalid GPU timestamp duration");
            auto& report = Report();
            {
                std::scoped_lock lock{report.mutex};
                report.intervals.emplace_back(begin, begin + ms);
                report.stream_ms[present] += ms;
                ++report.stream_submissions[present];
                report.Add(present ? "gpu_present_submission_ms" : "gpu_main_submission_ms", ms);
                // One submission per phase (BB_PERF_PHASES): phase times add up per frame.
                if (report.phases && !present) {
                    if (slot.frame != collected_frame) {
                        // Work before the first guest frame belongs to no frame.
                        if (collected_frame) FinishFrame(report);
                        else frame_ms = {};
                        collected_frame = slot.frame;
                    }
                    frame_ms[unsigned(slot.phase)] += ms;
                }
            }
            slot = {};
            pending.pop_front();
        }
    }
};

GpuStream::GpuStream(VkPhysicalDevice physical, VkDevice device, uint32_t family,
                     VkSemaphore completion, bool present) : impl{std::make_unique<Impl>()} {
    impl->device = device; impl->completion = completion; impl->present = present;
    VkPhysicalDeviceProperties properties{}; VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceProperties(physical, &properties);
    uint32_t count = 0; VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
    if (family >= count || !families[family].timestampValidBits ||
        !std::isfinite(properties.limits.timestampPeriod) || properties.limits.timestampPeriod <= 0)
        Fail("selected MoltenVK queue does not support valid timestamps");
    const unsigned bits = families[family].timestampValidBits;
    impl->mask = bits < 64 ? (uint64_t(1) << bits) - 1 : ~uint64_t(0);
    impl->period = properties.limits.timestampPeriod;
    VkQueryPoolCreateInfo create{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    create.queryType = VK_QUERY_TYPE_TIMESTAMP; create.queryCount = Impl::Slots * 2;
    Check(VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateQueryPool(device, &create, nullptr, &impl->pool),
          "vkCreateQueryPool");
    auto& report = Report(); std::scoped_lock lock{report.mutex};
    std::fprintf(report.file, "{\"type\":\"queue\",\"present\":%s,\"family\":%u,\"timestamp_valid_bits\":%u,"
        "\"timestamp_period_ns\":%.9f}\n", present ? "true" : "false", family, bits, impl->period);
    report.Flush();
}

GpuStream::~GpuStream() {
    // Submitted timestamp writes must finish before the pool goes away. The scheduler does not
    // wait for the GPU on destruction, so wait here (teardown only) and keep the last results.
    if (!impl->pending.empty()) {
        VkSemaphoreWaitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        wait.semaphoreCount = 1;
        wait.pSemaphores = &impl->completion;
        wait.pValues = &impl->last_tick;
        Check(VULKAN_HPP_DEFAULT_DISPATCHER.vkWaitSemaphores(impl->device, &wait, 5'000'000'000ull),
              "vkWaitSemaphores (timestamped work at renderer teardown, 5 s)");
        // The timestamps may resolve after the timeline; Collect fails loudly after 2 s.
        while (impl->Collect(), !impl->pending.empty())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    VULKAN_HPP_DEFAULT_DISPATCHER.vkDestroyQueryPool(impl->device, impl->pool, nullptr);
    auto& report = Report(); std::scoped_lock lock{report.mutex};
    report.Print(Clock::now(), true);
}

void GpuStream::Begin(VkCommandBuffer command, Phase phase, uint64_t frame) {
    impl->Collect();
    for (unsigned n = 0; n < Impl::Slots; ++n) {
        const unsigned i = (impl->next + n) % Impl::Slots;
        auto& slot = impl->slots[i];
        if (slot.used) continue;
        slot = {.used = true, .tick = 0, .frame = frame, .phase = impl->present ? Phase::Present : phase};
        impl->active = i;
        impl->next = (i + 1) % Impl::Slots;
        VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdResetQueryPool(command, impl->pool, i * 2, 2);
        VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, impl->pool, i * 2);
        return;
    }
    Fail("timestamp ring exhausted (512 submissions in flight); measurement would need a GPU stall");
}

void GpuStream::End(VkCommandBuffer command) {
    VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, impl->pool, impl->active * 2 + 1);
}

void GpuStream::Submitted(uint64_t tick) {
    impl->slots[impl->active].tick = tick;
    impl->pending.push_back(impl->active);
    impl->last_tick = tick;
}

RecorderTimer::RecorderTimer() { if (Enabled()) start = ThreadCpu(); }
RecorderTimer::~RecorderTimer() {
    if (!start) return;
    recorder_cpu.fetch_add(ThreadCpu() - start, std::memory_order_relaxed);
    recorder_chunks.fetch_add(1, std::memory_order_relaxed);
}

void ImportWait(uint64_t nanoseconds) {
    if (!Enabled()) return;
    import_ns.fetch_add(nanoseconds, std::memory_order_relaxed);
    import_count.fetch_add(1, std::memory_order_relaxed);
}

void Flip() {
    if (!Enabled()) return;
    auto& report = Report();
    const auto now = Clock::now();
    const auto c = ReadCounters();
    std::scoped_lock lock{report.mutex};
    ++report.flips;
    if (report.last_flip != Clock::time_point{}) {
        report.Add("guest_flip_interval_ms", std::chrono::duration<double, std::milli>(now - report.last_flip).count());
        const auto delta = [](const char* name, uint64_t now, uint64_t previous) {
            if (now < previous) {
                std::fprintf(stderr, "macOS performance measurement: counter %s went backwards: "
                                     "another consumer reset it\n", name);
                std::abort();
            }
            return now - previous;
        };
        report.Add("liverpool_cpu_ms", delta("liverpool", c.liverpool, report.previous.liverpool) / 1e6);
        report.Add("recorder_chunk_cpu_ms", delta("recorder", c.recorder, report.previous.recorder) / 1e6);
        report.Add("liverpool_idle_wall_ms", delta("idle", c.idle, report.previous.idle) / 1e6);
        report.Add("fence_wait_wall_ms", delta("fences", c.fences, report.previous.fences) / 1e6);
        report.Add("deferred_signal_wait_wall_ms", delta("deferred_signals", c.deferred_signals, report.previous.deferred_signals) / 1e6);
        report.Add("host_copy_record_wait_wall_ms", delta("copy_record", c.copy_record, report.previous.copy_record) / 1e6);
        report.Add("copy_worker_wait_wall_ms", delta("copy_workers", c.copy_workers, report.previous.copy_workers) / 1e6);
        report.Add("record_sync_wait_wall_ms", delta("record_wait", c.record_wait, report.previous.record_wait) / 1e6);
        report.Add("import_growth_wait_wall_ms", delta("import", c.import, report.previous.import) / 1e6);
        report.Add("import_growth_wait_count", delta("imports", c.imports, report.previous.imports));
        report.Add("recorder_chunk_count", delta("chunks", c.chunks, report.previous.chunks));
    }
    report.previous = c;
    report.last_flip = now;
    report.Print(now);
}
} // namespace BbPerf
#endif
