// SPDX-License-Identifier: GPL-2.0-or-later
// BB_PERF plumbing through the real Scheduler hooks (docs/macos/perf.md): configuration errors
// abort, every submission is measured once, flips are counted, the final window is written at
// teardown, and BB_PERF_PHASES gives one submission per phase and per-frame phase samples.
// Timestamp accuracy itself is tests/macos_perf/timestamps.sh.
#ifndef NDEBUG
#define NDEBUG
#define BB_RESTORE_TEST_ASSERTS
#endif
#include "bbport_toggles.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#ifdef BB_RESTORE_TEST_ASSERTS
#undef NDEBUG
#endif
#include <cassert>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

// Runs `body` in a child process (the settings are read once per process); returns its status.
template <typename Body>
static int InChild(Body&& body) {
    std::fflush(nullptr);
    const pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        const struct rlimit limit{0, 0};
        setrlimit(RLIMIT_CORE, &limit);
        body();
        std::fflush(nullptr);
        _exit(0);
    }
    int status = 0;
    assert(waitpid(child, &status, 0) == child);
    return status;
}
static bool Aborted(int status) {
    return WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
}
static bool Passed(int status) {
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
static void Env(const char* perf, const char* out, const char* phases) {
    for (auto [name, value] : {std::pair{"BB_PERF", perf}, {"BB_PERF_OUT", out}, {"BB_PERF_PHASES", phases}}) {
        if (value) setenv(name, value, 1);
        else unsetenv(name);
    }
    unsetenv("BB_GPU_PROFILE");
    unsetenv("BB_PERF_FRAMES");
    setenv("BB_FRAME_STATS", "1", 1);
}
static std::vector<std::string> Lines(const std::string& path) {
    std::ifstream file(path);
    std::vector<std::string> lines;
    for (std::string line; std::getline(file, line);) lines.push_back(line);
    return lines;
}
static bool Has(const std::string& line, const std::string& text) {
    return line.find(text) != std::string::npos;
}
// The integer after `"key":` in a JSON line (the test's own records only).
static long long Number(const std::string& line, const std::string& key) {
    const auto at = line.find("\"" + key + "\":");
    assert(at != std::string::npos);
    return std::atoll(line.c_str() + at + key.size() + 3);
}

// A presenter-like draw scheduler: frames of scene/post/upscaler work, one flip each.
static void Run(int frames) {
    Vulkan::Instance instance(0, false);
    Vulkan::Scheduler scheduler(instance);
    scheduler.InitPerf(false);
    for (int frame = 1; frame <= frames; ++frame) {
        scheduler.PerfMark(BbPerf::Phase::Scene, frame);
        scheduler.Flush();
        scheduler.PerfMark(BbPerf::Phase::Post);
        {
            Vulkan::Scheduler::PerfScope upscaler{scheduler, BbPerf::Phase::Upscaler};
        }
        scheduler.Flush();
        BbStats::deferred_signals_wait_ns.fetch_add(2000000);
        BbStats::host_copy_record_wait_ns.fetch_add(3000000);
        BbStats::copy_threads_wait_ns.fetch_add(5000000);
        BbPerf::Flip();
    }
    scheduler.Finish();
}

// Reads each submission's timestamps right after its timeline value is reached, without
// Finish(): MoltenVK can signal the timeline before the results resolve (VK_NOT_READY), and
// those reads must be retried, not fail, and every submission still measured exactly once.
static void RunTight(int submissions) {
    Vulkan::Instance instance(0, false);
    Vulkan::Scheduler scheduler(instance);
    scheduler.InitPerf(false);
    for (int i = 0; i < submissions; ++i) {
        const u64 tick = scheduler.CurrentTick();
        scheduler.Flush();
        scheduler.Wait(tick); // the next Flush's Begin reads these results immediately
    }
    BbPerf::Flip();
}

int main() {
    char dir_template[] = "/tmp/bbport-perf-test-XXXXXX";
    assert(mkdtemp(dir_template));
    const std::string dir = dir_template;
    const std::string existing = dir + "/existing.jsonl";
    std::fclose(std::fopen(existing.c_str(), "w"));

    // Configuration errors abort before any measurement.
    assert(Aborted(InChild([] { Env("2", nullptr, nullptr); (void)BbPerf::Enabled(); })));
    assert(Aborted(InChild([] { Env("1", nullptr, nullptr); (void)BbPerf::Enabled(); })));
    assert(Aborted(InChild([&] { Env("1", existing.c_str(), nullptr); (void)BbPerf::Enabled(); })));
    assert(Aborted(InChild([] { Env(nullptr, nullptr, "1"); (void)BbPerf::Enabled(); })));
    assert(Aborted(InChild([] {
        Env(nullptr, nullptr, nullptr);
        setenv("BB_PERF_FRAMES", "1", 1);
        (void)BbPerf::Enabled();
    })));
    assert(Aborted(InChild([&] { Env("1", (dir + "/x.jsonl").c_str(), "yes"); (void)BbPerf::Phases(); })));
    assert(Aborted(InChild([&] {
        Env("1", (dir + "/y.jsonl").c_str(), nullptr);
        setenv("BB_GPU_PROFILE", "1", 1);
        (void)BbPerf::Enabled();
    })));
    // A consumer resetting a cumulative total must fail instead of producing an underflow.
    const std::string reset = dir + "/reset.jsonl";
    assert(Aborted(InChild([&] {
        Env("1", reset.c_str(), nullptr);
        BbStats::deferred_signals_wait_ns.store(1000000);
        BbPerf::Flip();
        BbStats::deferred_signals_wait_ns.store(0);
        BbPerf::Flip();
    })));
    std::remove(reset.c_str());
    // Off: no file, no measurement, the hooks still run.
    assert(Passed(InChild([] { Env("0", nullptr, nullptr); assert(!BbPerf::Enabled()); Run(2); })));

    constexpr int Frames = 4;
    // Unperturbed: one timed span per real submission, no extra submissions.
    const std::string plain = dir + "/plain.jsonl";
    assert(Passed(InChild([&] { Env("1", plain.c_str(), "0"); Run(Frames); })));
    auto lines = Lines(plain);
    assert(lines.size() == 3); // schema, queue, final window
    assert(Has(lines[0], "\"type\":\"schema\"") && Has(lines[0], "\"perturbed\":false"));
    assert(Number(lines[0], "version") == 3);
    assert(Has(lines[0], "\"frame_samples\":false") && !Has(lines[2], "\"flip_samples\""));
    assert(Has(lines[1], "\"type\":\"queue\"") && Has(lines[1], "\"present\":false"));
    assert(Has(lines[2], "\"final\":true") && Has(lines[2], "\"perturbed\":false"));
    assert(Number(lines[2], "flips") == Frames);
    // Run() submits twice per frame; Finish() submits once more, and on Apple the scheduler's
    // destructor closes it, submitting the open command buffer (Scheduler::Close): every real
    // submission is one timed span.
    constexpr int Submissions = 2 * Frames + 2;
    assert(Number(lines[2], "main_submissions") == Submissions);
    assert(Number(lines[2], "present_submissions") == 0);
    assert(Has(lines[2], "\"gpu_main_submission_ms\":{\"n\":" + std::to_string(Submissions)));
    assert(Has(lines[2], "\"guest_flip_interval_ms\":{\"n\":" + std::to_string(Frames - 1)));
    assert(!Has(lines[2], "gpu_scene_ms"));
    for (const char* name : {"deferred_signal_wait_wall_ms", "host_copy_record_wait_wall_ms",
                             "copy_worker_wait_wall_ms"})
        assert(Has(lines[2], std::string("\"") + name + "\":{\"n\":" + std::to_string(Frames - 1)));
    assert(!Has(lines[2], "\"host_copy_wait_wall_ms\""));
    assert(Number(lines[2].substr(lines[2].find("\"deferred_signal_wait_wall_ms\"")), "mean") == 2);
    assert(Number(lines[2].substr(lines[2].find("\"host_copy_record_wait_wall_ms\"")), "mean") == 3);
    assert(Number(lines[2].substr(lines[2].find("\"copy_worker_wait_wall_ms\"")), "mean") >= 5);

    const std::string raw = dir + "/raw.jsonl";
    assert(Passed(InChild([&] {
        Env("1", raw.c_str(), "0");
        setenv("BB_PERF_FRAMES", "1", 1);
        Run(Frames);
    })));
    lines = Lines(raw);
    assert(lines.size() == 3 && Has(lines[0], "\"frame_samples\":true"));
    assert(Number(lines[2], "main_submissions") == Submissions);
    assert(Number(lines[2], "flip_sequence_end") == Frames);
    const auto sample_at = lines[2].find("\"flip_samples\":[");
    assert(sample_at != std::string::npos);
    const char* sample = lines[2].c_str() + sample_at + std::string("\"flip_samples\":[").size();
    unsigned long long previous = 0;
    for (int i = 0; i < Frames - 1; ++i) {
        unsigned long long end = 0, duration = 0;
        int consumed = 0;
        assert(std::sscanf(sample, "[%llu,%llu]%n", &end, &duration, &consumed) == 2);
        assert(duration > 0 && end >= duration && consumed > 0);
        if (previous) assert(end - previous == duration);
        previous = end;
        sample += consumed;
        if (i < Frames - 2) assert(*sample++ == ',');
    }
    assert(*sample == ']');

    // Phases: each phase change submits. Per frame: Scene (mark + flush), Post, Upscaler,
    // back to Post on scope exit, then the explicit flush (nothing new): 4 submissions.
    const std::string phases = dir + "/phases.jsonl";
    assert(Passed(InChild([&] { Env("1", phases.c_str(), "1"); Run(Frames); })));
    lines = Lines(phases);
    assert(lines.size() == 3);
    assert(Has(lines[0], "\"perturbed\":true") && Has(lines[2], "\"perturbed\":true"));
    assert(Number(lines[2], "flips") == Frames);
    const long long submissions = Number(lines[2], "main_submissions");
    std::printf("BB_PERF phases: %lld submissions for %d frames\n", submissions, Frames);
    assert(submissions > Submissions); // phases add submissions to the unperturbed run
    // A frame's phase samples are complete when the next frame's first submission is read:
    // all but the last frame.
    for (const char* name : {"gpu_scene_ms", "gpu_post_ms", "gpu_upscaler_ms", "gpu_frame_work_ms"})
        assert(Has(lines[2], std::string("\"") + name + "\":{\"n\":" + std::to_string(Frames - 1)));

    // Reads immediately after the timeline: deferred, never lost or doubled.
    constexpr int Tight = 400;
    const std::string tight = dir + "/tight.jsonl";
    assert(Passed(InChild([&] { Env("1", tight.c_str(), nullptr); RunTight(Tight); })));
    lines = Lines(tight);
    assert(lines.size() == 3);
    assert(Number(lines[2], "main_submissions") == Tight + 1); // + the destructor's Close
    const long long deferred = Number(lines[2], "deferred_reads");
    std::printf("BB_PERF read-after-timeline: %d submissions, %lld deferred reads\n", Tight, deferred);

    std::remove(tight.c_str());
    std::remove(raw.c_str());
    std::remove(existing.c_str());
    std::remove(plain.c_str());
    std::remove(phases.c_str());
    rmdir(dir.c_str());
    std::puts("BB_PERF: configuration errors, per-submission spans, flips, final window, phases PASS");
}
