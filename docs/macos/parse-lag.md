# Loading and character-creation parse lag

The strongest measured bottleneck is stage A waiting for downstream draw work,
not decoding PM4 instructions. Cold shader/pipeline creation adds a separate,
large entry hitch. Import growth is not implicated by the inspected logs. The
remaining question is how stage B's elapsed time divides between CPU work,
recorder/fence waits and GPU progress; current counters cannot separate it fully.

## Evidence and timing

Primary run: Playtest's `20261010-190324-1080p-ACCEPT-gpuidle-9668c1c`, default
configuration, cold cache. `game.log` has no timestamps. UTC intervals below come
from `events.jsonl` capture timestamps and recorded `log_lines`; they bracket
observations, not exact event timestamps. Load average rises from 11.8 at
23:05:25 to 23.1 at 23:05:58, so this is not a quiet performance baseline.

| Phase / UTC bracket | Queue statistic | Supporting evidence in game.log |
| --- | --- | --- |
| Title through movie, before 23:04:53 | Mean 1.9–2.1, max 5–7 | L370–633; roughly 4,270 graphics submissions per reporting window |
| Movie end to creation, 23:04:53–23:05:25 | Mean 13.4, max 74 | L816; 9,413 submissions. L773: 666.7 ms frame, 619.4 ms Liverpool CPU; L783: 87 shader/pipeline compile events, 620.6 ms in that stats window |
| Steady creation, after 23:05:25 | Mean 20.2, max 48 | L1282; 28,600 submissions. L850/931/1068/1264: zero compiles, 201–202 draws/frame, 30–44 FPS |
| Creation/finalise/world transition, through and after 23:05:58 | Mean 14.7, max 49 | L1992; 21,331 submissions. L1577/1800: 110 / 16 compile events, 274.7 / 225.7 ms |

These counters need careful interpretation. `SubmitGfx` samples global
`num_submits` **after** incrementing it. This includes the newly enqueued task,
the currently active task and asynchronous-compute tasks, not only unparsed
older graphics submissions. “600 frames” counts `submit_done` observations in
`Liverpool::Process`, not verified presentation frames. Submissions per reporting
interval rise about 6.7x from light scenes to steady creation; depth in submissions
is therefore not comparable to latency in milliseconds without queue-age data.

The later `8cf9ed7` ACCEPT2 run's mean 2.0–2.2 / max 7–9 covers title/movie only:
it panics at movie EOF before the comparable heavy scene. It does not demonstrate
that heavy-scene queue depth improved.

## Ranked causes

1. **Sustained stage-B/retirement backpressure: directly measured.** At L858,
   stage A drains consume 83.3% of the elapsed TSC window, with `OnSubmit` alone
   80.2%; stage B handler elapsed is 91.1%. At L1191, A drains take 67.3%, split
   between `OnSubmit` 52.5% and final `Process` retirement 14.9%; B handler elapsed
   is 60.9%. No shaders compile in those nearby frame windows. During world load,
   L1924 shows A drains 86.4%: `ProcessCeUpdate` / `DumpConstRam` 47.4%, `OnSubmit`
   28.2%, retirement 10.7%. Stage A cannot parse while those drains run.
   These percentages describe elapsed waits, not CPU utilization, and overlapping
   counters must not be added together.
2. **Cold compilation on stage A: directly supported for entry hitches.**
   `Rasterizer::Draw` selects/creates a graphics pipeline before posting the draw
   to stage B. Cache misses synchronously execute `CompileModule` and construct
   `GraphicsPipeline`; compute selection similarly happens on A after a drain.
   Draw-preparation workers only reuse known matching modules: they mark a miss
   failed rather than compiling it. The 619 ms CPU frame and adjacent 621 ms
   compile window support a compile-dominated entry hitch, but do not explain
   the later zero-compile backlog. Tiling pipelines can also compile on the
   cache-owning thread and are not necessarily represented by that compile timer.
3. **Recorder completion, GPU waits and scheduling amplify downstream waits.**
   Creation frame windows report GPU-tick waits of 26.9–30.0% early, then roughly
   2–5%; the world transition also reaches 27.4%. “Host copies” reaches 10–23%,
   but is an ambiguous label: `WaitDeferredSignals` increments the same timer as
   copy-record completion. Actual copy-worker waits round to 0.0%, and copy-record
   wait counts round to zero/frame. This cannot be called a memcpy bottleneck.
   Stage-A drain spins then yields; stage B spins up to 200 microseconds when
   idle on this host. Host contention and wake scheduling are plausible amplifiers,
   not separately measured causes. Speculative preparation has low hit rates in
   several late windows despite eight background workers.
4. **Import growth / pure Rosetta parser cost: not demonstrated.** There are
   zero `macOS host import overlap wait` lines in either inspected run; that
   completion-boundary path logs every invocation. First import/allocation cost
   is a separate, unmeasured possibility. Apple buffers avoid host shadow copies,
   but images still require uploads/detiling (some logged frames upload 120 MB).
   No counter isolates Rosetta translation or PM4-only execution. Repeated
   zero-compile drain waits are stronger evidence than a translation hypothesis.

## Proposed fixes and minimal next measurement

Keep snapshot ownership and guest-visible completion ordering intact. Do not
remove `GpuIdle` retirement checks to reduce a queue-depth number.

- Prewarm validated shader/pipeline caches and consider bounded parallel creation
  from immutable specialization snapshots, preserving command order at the first
  consumer. This targets cold hitches without skipping draws.
- Measure and optimize stage B first for steady throughput. Investigate narrower
  dependency drains or immutable per-draw constant snapshots for `DumpConstRam`;
  remove a drain only after proving every affected guest read/write is ordered.
- Split `host_copies_wait_ns` into recorder-deferred signal wait and host-copy
  record wait; retain the separate `BbCopy::WaitAsync` timer. Label B “handler
  elapsed”, and measure its thread CPU time separately at coarse windows.
- Add opt-in per-submission enqueue/first-resume/done timestamps, graphics/compute
  counts, bytes and age percentiles. Add a coarse active phase for A (parse,
  compile, B-drain, retirement) with max-age snapshots. This needs no per-PM4-op
  clock read. Include draw-ring-full wait, currently absent from drain totals.
- Use the same monotonic window boundaries for these counters and `BB_PERF` GPU
  spans. Compare cold/warm runs of the same heavy scene under recorded quiet
  load. Native host or Rosetta loop optimization comes after separating actual
  parser CPU from compilation and downstream waiting.

The accompanying measurement change implements the wait-counter split and the
stage-B label above. It adds no clock reads or waits. `BB_PERF` schema 3 reports
deferred-signal and copy-record waits separately. Frame stats now take deltas
without resetting these shared counters: resetting them corrupted simultaneous
BB_PERF deltas. A backwards counter now fails loudly. The other proposed counters
and scheduling changes remain future work.

Source audit: `liverpool.cpp` Process/SubmitGfx/ProcessCeUpdate;
`vk_rasterizer.cpp` Draw/OnSubmit/PrintPipeStats; `vk_draw_pipe.h` Drain/Run;
`vk_pipeline_cache.cpp` GetProgram/GetGraphicsPipeline/GetComputePipeline;
`vk_scheduler.cpp` WaitDeferredSignals/WaitHostCopies; Apple GetArena in
`buffer_cache.cpp`. The measurements above come from existing logs; no new game runs were made for this analysis.
