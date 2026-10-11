// Overhead of the always-on parse split (bbport_parse_split.h), measured as process CPU time
// for realistic stage A packet and stage B draw sequences, accounting on vs off (no account: the
// production cost for threads without one is the thread_local load and a branch).
// Build and run: tests/bench_parse_split.sh (x86_64, under Rosetta on Apple Silicon).
#include <cstdint>
#include <cstdio>
#include <ctime>
#include "bbport_parse_split.h"

namespace {
volatile std::uint64_t sink = 0;

// Stand-in for a packet's own work, so the compiler cannot fold the loop.
inline void Work(std::uint32_t n) {
    std::uint64_t x = sink;
    for (std::uint32_t i = 0; i < n; ++i) {
        x = x * 6364136223846793005ull + 1442695040888963407ull;
    }
    sink = x;
}

double CpuNs() {
    timespec ts{};
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return ts.tv_sec * 1e9 + ts.tv_nsec;
}

// Stage A: per draw, 6 register packets, 1 draw packet and 1 label-ish packet. Mark per packet,
// as liverpool.cpp does (same-bucket marks are no-ops), plus the idle-drain check (no scope).
void StageAFrame(std::uint32_t draws) {
    using namespace BbSplit;
    for (std::uint32_t d = 0; d < draws; ++d) {
        for (int r = 0; r < 6; ++r) {
            Mark(ARegisters);
            Work(8);
        }
        Mark(ADraw);
        Work(40);
        Mark(ALabels);
        Work(8);
    }
}

// Stage B: one draw packet with the marks and sections of DrawPipe::Run, RunDrawPacket and
// DrawRecord, plus `scopes` scopes (Renderer's recording split adds ~15 per draw).
void StageBDraw(std::uint32_t scopes) {
    using namespace BbSplit;
    Mark(BDecode);
    Work(10);
    MarkOn(stage_b, BDrawSetup);
    Work(5);
    MarkOn(stage_b, BRenderState);
    Work(10);
    MarkOn(stage_b, BBind);
    {
        Section buffers{stage_b, BBuffers};
        Work(20);
    }
    {
        Section textures{stage_b, BTextures};
        Work(20);
    }
    MarkOn(stage_b, BBegin);
    Work(10);
    MarkOn(stage_b, BRecord);
    for (std::uint32_t i = 0; i < scopes; ++i) {
        Scope s{KDeferred};
        Work(2);
    }
    Mark(BAfter);
    Mark(BIdle);
}

template <class F>
double Measure(F&& f, std::uint32_t reps) {
    const double t0 = CpuNs();
    for (std::uint32_t i = 0; i < reps; ++i) {
        f();
    }
    return (CpuNs() - t0) / reps;
}
} // namespace

int main() {
    using namespace BbSplit;
    constexpr std::uint32_t frames = 2000, draws = 800, reps = 2'000'000;
    for (int round = 0; round < 3; ++round) {
        current = nullptr;
        const double a_off = Measure([] { StageAFrame(800); }, frames);
        Attach(stage_a, ALoop);
        const double a_on = Measure([] { StageAFrame(800); }, frames);
        current = nullptr;
        const double b_off = Measure([] { StageBDraw(0); }, reps);
        const double b15_off = Measure([] { StageBDraw(15); }, reps);
        Attach(stage_b, BIdle);
        const double b_on = Measure([] { StageBDraw(0); }, reps);
        const double b15_on = Measure([] { StageBDraw(15); }, reps);
        current = nullptr;
        const double packets = draws * 8.0;
        std::printf("round %d: stage A %.2f ns/packet added (%.0f vs %.0f us/frame of 800 draws); "
                    "stage B %.1f ns/draw added with PM4's marks, %.1f ns/draw with 15 more "
                    "scopes (%.0f vs %.0f ns/draw)\n",
                    round, (a_on - a_off) / packets, a_on / 1e3, a_off / 1e3, b_on - b_off,
                    b15_on - b15_off, b15_on, b15_off);
    }
    // Raw costs.
    const double rdtsc = Measure([] { sink = sink + __rdtsc(); }, 10'000'000);
    current = nullptr;
    const double base = Measure([] { sink = sink + 1; }, 10'000'000);
    std::printf("rdtsc + loop %.2f ns, loop alone %.2f ns\n", rdtsc, base);
    return 0;
}
