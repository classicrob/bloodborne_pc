# QW2 wait attribution and sleep qualification

## Raw-log correction

Run A used shared 2a874fd, warm 1080p clinic-start, GS draws skipped, and the
aggregate default filter. Its raw log contains substantial waits for both
`main` and `GXRenderThread`; the initial report that those threads had no
tracked waits was a parsing omission.

Evidence is in Opus-Build's worktree parent:
`bbport-edit/out/macos_route/20261011-034423-1080p-QW2-A-1080p-2a874fd-gsskip/game.log`.
The final window begins at line 13928 and covers 60 flips / 19,217.141 ms.

| Thread / import | Completed | Call wall ms | Window share |
| --- | ---: | ---: | ---: |
| GXRenderThread / scePthreadCondWait | 61 | 12,114.866 | 63.042% |
| GXRenderThread / sceGnmSubmitCommandBuffers | 673 | 2,475.207 | 12.880% |
| GXRenderThread / sceGnmSubmitDone | 60 | 0.613 | 0.003% |
| main / scePthreadCondWait | 362 | 2,557.578 | 13.309% |
| main / sceKernelWaitSema | 1,668 | 1,328.755 | 6.914% |
| NexusRevolution Main / sceKernelUsleep | 1,258 | 17,651.885 | 91.855% |

GX's preceding two windows show CondWait at 67.922% and 43.179%, and
SubmitCommandBuffers at 8.804% and 25.313%. Thus the condvar handoff and submit
path are already concrete leads, before enabling mutex tracing. Pending-state
rows identify the same pthreads; this is not a name-cache mismatch. Durations
are inclusive and charged when calls complete, as described in
[the trace documentation](hle-trace.md); they do not directly measure CPU sleep
or prove which other thread satisfies a wait.

The default `waits` filter already includes semaphore waits. `sync` adds mutex,
clock and yield hot paths, among others. Native guest-linked functions and guest
spin loops are outside the runtime import wrapper.

## Instrumentation cost in this run

The last three windows contain 20,779 / 21,009 / 23,713 completed selected calls,
about 925–1,234 per second. Applying the synthetic 0.357 microsecond added CPU
cost per call gives about 0.124–0.141 ms CPU per flip. This is an extrapolation,
not a gameplay A/B: it excludes reporting I/O, scheduling/cache effects and any
change in game behavior. It makes direct per-call bookkeeping an unlikely
explanation for an extra 150 ms/frame, but the matched no-summary control still
matters. That control stopped at `MapNumberConversion data_fmt=7`, so there is
no clean causal comparison yet.

## Next game-run specification

Build schedules a paired summary/no-summary run on the same pinned artifact,
settings, warm cache and 1080p clinic-start fixture, retaining GS skip consistently
if still required. Keep the raw HLE lines and collect at least three complete
60-flip steady windows spanning still and walking phases.

Summary member:

```sh
BB_PERF=1
BB_HLE_TRACE_SUMMARY=60
BB_HLE_TRACE_SUMMARY_FILTER=sync,gnm,videoout,avplayer
```

Unset `BB_HLE_TRACE`, `BB_HLE_TRACE_STREAM` and `BB_CPU_PROFILE`. The control also
unsets `BB_HLE_TRACE_SUMMARY` and its filter. Broad `sync` is explicitly more
intrusive than the default; use it for attribution, not an unqualified FPS claim.
The next questions are the remaining main-thread mutex time and the condvar
handoff, not whether GX has any tracked wait at all.

## Sleep latency measurement tool

Run after a quiet window, through the quick pool:

```sh
tools/macos/heavy.sh --pool quick usleep-latency -- \
  sh tools/macos/usleep_probe/run.sh 64
```

This is an on-demand performance measurement, not a pass/fail timing test in
`--test`. The signed x86_64 executable includes the production
`src/runtime_kernel.c` and directly invokes its `kernel_usleep` implementation;
unrelated runtime sections are removed at link. It compares that function with
host `nanosleep` under Rosetta. A second signed arm64 executable measures only
host `nanosleep`. Output contains requested duration, sample count, minimum,
p50/p95/p99/max, mean, mean excess and early-return count. Both controls retry
EINTR like the production helper. No signals or artificial CPU load are added.

Each duration has two warmup calls and 64 measured samples by default, for
0/1/10/100/500 microseconds and 1/5/10/20/50 milliseconds. Timings use
`mach_absolute_time` with its returned timebase. The output directory records
source commit and host load before/between/after. Runs are sequential, not
simultaneous; changing system contention can affect their comparison. No guest
QoS/priority equivalence is claimed.

Source inspection confirms the request is widened to uint64 before multiplying
microseconds by 1,000, then split into seconds/nanoseconds for `nanosleep`.
There is no microsecond/millisecond conversion error on this path. An aggregate
Usleep duration alone does not reveal the requested N: the roughly 14 ms mean
for NexusRevolution Main cannot establish oversleep without its arguments.
A future short exact-import trace of `sceKernelUsleep` can supply those arguments
if that thread is shown to lie on the gameplay critical path.

## Measured sleep results

Signed Rosetta and native controls completed after Build ended the quiet window.
The source of `runtime_kernel.c` is byte-identical to shared 2a874fd. All cases
returned successfully and recorded zero early returns.

Second run, 64 samples/case (times in microseconds):

| Requested | Guest Usleep p50 | Guest p95 | Guest max | Rosetta host nanosleep p50 | Native arm64 nanosleep p50 |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 10.625 | 32.500 | 46.083 | 11.500 | 13.125 |
| 10 | 23.709 | 35.041 | 84.166 | 24.541 | 23.042 |
| 100 | 180.958 | 745.542 | 1,948.625 | 175.542 | 168.708 |
| 500 | 811.916 | 2,001.000 | 2,779.500 | 788.708 | 908.375 |
| 1,000 | 1,618.125 | 3,614.500 | 6,661.625 | 1,564.125 | 1,588.958 |
| 5,000 | 7,200.083 | 8,432.458 | 9,617.333 | 7,484.333 | 7,522.708 |
| 10,000 | 14,956.875 | 16,125.875 | 17,638.333 | 14,465.000 | 14,034.334 |
| 20,000 | 27,754.000 | 30,959.750 | 35,368.042 | 28,162.208 | 26,652.916 |
| 50,000 | 58,996.375 | 61,493.125 | 63,586.166 | 58,692.375 | 58,194.916 |

Evidence: `out/usleep-probe/run.9NKFGs/`, including the two CSV files and host-load
snapshots. One-minute load averages were 158–173. The first Rosetta-only run
`out/usleep-probe/run.K8YJsp/` at load 108–112 was more variable: 10 ms and 20 ms
requests had guest medians 15.49 ms and 29.74 ms, versus host medians 15.29 ms and
30.54 ms. Sequential, contended runs cannot separate timer coalescing from
scheduler delay or quantify a Rosetta-specific penalty.

The evidence establishes real excess wakeup latency for these requests under
current conditions, but no excess specific to our microsecond conversion:
Rosetta host and native controls oversleep too. A polling loop could be stretched
by that latency. Connecting it to frame time still requires the game's requested
N and evidence that the sleeping thread gates rendering. Do not shorten requested
sleeps, change QoS, or introduce a busy wait based on these measurements alone.
