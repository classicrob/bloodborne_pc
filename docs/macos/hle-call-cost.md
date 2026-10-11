# HLE call cost and main/render-thread attribution

## What an import call does

`probe.c` resolves each imported relocation before guest execution and writes the
resolved function address into the guest image. With tracing disabled,
`runtime_resolve` returns the original target. The guest PLT/GOT call enters an
x86-64 SysV function in the runtime or GPU library; both caller and callee execute
under Rosetta. There is no per-call symbol lookup, guest-to-ARM ABI bridge, or
low-stack switch here. Darwin library calls and syscalls inside an import body
can have their own costs. The low-stack helper is for host threads invoking guest
callbacks, not every guest import.

The previously measured approximately 0.357 microsecond/call is the additional
CPU cost of **aggregate tracing**, not the intrinsic import boundary. Multiplying
that number by every import count would estimate enabled instrumentation, not
production HLE overhead.

## Measurement

`tools/macos/hle_cost/run.sh` builds a signed x86-64 measurement executable from
this checkout's production runtime sources and a caller-specified stable GPU
library. `BB_HLE_COST_INCLUDE_ROOT` defaults to this checkout and supplies the
LibAtrac9 submodule headers when they are populated in another checkout. It only
adds an include search path; runtime sources and the recorded revision always
come from this checkout. It does not
launch the game or initialize the GPU renderer. Run through the quick pool:

```sh
BB_HLE_COST_GPU=/absolute/path/to/pinned/libbbgpu.dylib \
BB_HLE_COST_ATRAC=/absolute/path/to/libatrac9.a \
BB_HLE_COST_INCLUDE_ROOT=/checkout/with/third_party/headers \
  tools/macos/heavy.sh --pool quick hle-cost -- \
  bash tools/macos/hle_cost/run.sh
```

A generated low-address executable stub uses the resolved guest PLT instruction
`jmp qword [rip+displacement]` through a GOT target. The trivial no-PLT and PLT
controls use the same noinline callee and function-pointer call loop. Both use
the harness caller and stack; only the generated jump stub is low-address.
Other rows resolve real NIDs once
and call the actual exported HLE function, including its error-wrapper checks and
bookkeeping. TLS and object allocation are warmed before measurement. No summary,
ring trace or CPU sampling is enabled.

Each row has one million iterations in five passes, with order reversed in
alternate passes. CPU is process user+system time from `getrusage`; elapsed time
is measured separately with `mach_absolute_time`. CPU values include the loop,
indirect call, return and a volatile result store. The PLT row is a **warm
complete call-loop cost**, not an isolated Rosetta boundary measurement. The
no-PLT row is also an indirect function-pointer call, not a direct call
instruction. Comparing them describes these two harness loops, not a universal
transition tax or a bound on cold/mispredicted calls.
Mutex lock/unlock and semaphore wait/signal rows are **pairs**; the tool never
pretends to separate their body costs. Condition signals have no waiters and
semaphore waits already have a token. Those are fast-path controls, not models of
contended game waits. HTTP is this backend's zero-timeout, no-events path.

Every row is checked through its actual target before and after each measured
loop, outside timing: status results, clock ranges, TLS bytes and pointer
identity/readability. Both pair members are checked; mutex busy/unlocked controls
and semaphore empty/full controls verify restored state. A successful run logs
that all 75 measurements passed both checks. The result directory records
`compiler.txt` and the exact expanded `compile-command.sh`, including flags.

Results are not game CPU measurements. Scheduling, CPU migration and Rosetta's
translation/cache behavior can differ in gameplay. Wall times on the loaded
machine especially cannot be substituted for per-call CPU cost.

## Measured CPU cost

Signed quick-pool run `out/hle-cost/run.ZrFAbP` completed all five passes on
2026-10-11. The production runtime files used here are byte-identical to their
2a874fd versions. The output directory retains CSV, raw stdout/stderr, copied
benchmark sources, source revision, pinned library hash, compiler version and
exact compile/link command. Compiler: Apple clang 21.0.0
(`clang-2100.0.123.102`), with explicit `-arch x86_64`; signing and all 75
pre/post validation pairs passed. One-minute host load
was 141–142, so these are loaded-host process-CPU measurements, not a quiet FPS
qualification. CPU samples were much more stable than elapsed samples.

| Case | Median CPU ns/iteration | Min–max CPU ns | Calls/iteration |
| --- | ---: | ---: | ---: |
| Trivial no-PLT call-loop control | 3.091 | 2.912–3.331 | 1 |
| Trivial PLT call-loop control | 6.271 | 6.218–6.492 | 1 |
| sceKernelClockGettime, monotonic (4) | 57.510 | 57.273–57.627 | 1 |
| sceKernelClockGettime, realtime (0) | 47.624 | 47.119–47.829 | 1 |
| sceKernelGetProcessTime | 53.846 | 53.356–54.436 | 1 |
| sceKernelReadTsc | 14.528 | 12.288–16.584 | 1 |
| sceKernelWaitSema + SignalSema, uncontended/token available | 37.000 | 36.611–38.420 | 2 |
| sceHttpWaitRequest, backend zero-timeout/no-events path | 5.571 | 5.149–5.802 | 1 |
| scePthreadSelf, attached thread | 11.221 | 10.536–11.430 | 1 |
| scePthreadGetspecific | 14.554 | 14.420–14.654 | 1 |
| __error | 11.878 | 11.629–12.283 | 1 |
| __tls_get_addr, allocated module | 9.726 | 9.163–9.939 | 1 |
| scePthreadMutexLock + Unlock, uncontended | 43.771 | 43.587–44.457 | 2 |
| scePthreadCondSignal, no waiters | 12.456 | 11.649–12.957 | 1 |
| scePthreadCondBroadcast, no waiters | 15.457 | 15.041–15.622 | 1 |

The previous run `run.GBm8JY` lacked the untimed return/state checks. Its
numbers are superseded by the validated run above; they must not be used as proof
that all timed paths succeeded. No row isolates import-body CPU from loop/call
cost. Different callee code and branch layout matter, so subtracting the trivial
control from every function is also unjustified.

For scale, 1,000 monotonic-clock calls cost about 0.058 ms CPU, 1,000 cached TLS
lookups about 0.010 ms, and 1,000 mutex lock/unlock pairs about 0.044 ms in this
microbenchmark. It would take about 17,388 monotonic-clock calls or 22,846 mutex
pairs **per frame** to reach 1 ms at these measured fast-path rates. Actual game
counts and contention are required before using those extrapolations.

## Existing gameplay evidence and coverage limits

The current analyzed capture is QW2-A, shared 2a874fd, warm 1080p clinic-start,
GS skipped, default `waits,gnm,videoout,avplayer` aggregate filter. Its last three
steady windows contain 180 flips in 61,268.127 ms. The main and GX rows contain
only nine distinct thread/import pairs, not ten. Mutex, clock, TLS and ordinary
nonselected imports were not counted. A global top-ten CPU-cost ranking cannot
be recovered from omitted counts.

Generate the observed ranking with:

```sh
python3 tools/macos/hle_cost/summary.py /path/to/game.log \
  --windows 3 --call-loop-ns MEASURED_WARM_PLT_LOOP_NS --instrument-ns 357
```

| Rank | Thread | Import | Calls/flip | Inclusive call wall ms/flip |
| ---: | --- | --- | ---: | ---: |
| 1 | GXRenderThread | scePthreadCondWait | 1.000 | 199.061 |
| 2 | GXRenderThread | sceGnmSubmitCommandBuffers | 11.194 | 52.280 |
| 3 | main | scePthreadCondWait | 5.722 | 39.114 |
| 4 | main | sceKernelWaitSema | 27.339 | 24.017 |
| 5 | GXRenderThread | sceGnmSubmitAndFlipCommandBuffers | 1.000 | 0.124 |
| 6 | main | sceHttpWaitRequest | 1.994 | 0.011 |
| 7 | GXRenderThread | sceKernelWaitSema | 0.006 | 0.009 |
| 8 | GXRenderThread | sceVideoOutGetFlipStatus | 1.000 | 0.004 |
| 9 | GXRenderThread | sceGnmSubmitDone | 1.000 | 0.004 |

Main contributes 35.056 observed calls/flip and GX 15.200. Multiplying these
counts by a warm trivial call-loop measurement is only a **hypothetical harness
workload projection**. It does not estimate their actual HLE CPU cost, blocked
or wakeup time, or gameplay totals. The imports execute different bodies; many
block, and omitted imports have unknown counts. `summary.py` labels this field
`hypothetical_trivial_call_loop_cpu_ms_per_flip`.

The JSON also records both assumed per-call inputs explicitly. Its default
357 ns instrumentation input comes from the earlier separate aggregate-trace
benchmark, not this run. Applying that input to the observed rows gives
0.012515 / 0.005426 ms per flip for main/GX bookkeeping, conditional on those
costs transferring. It excludes report I/O and scheduling effects. Full-sync
counts and the no-summary control remain necessary.

The zero-timeout HTTP control cannot be applied to the observed HTTP row without
establishing its timeout arguments. Neither condvar blocking nor real GPU
submissions can be modeled by the trivial or uncontended controls.

Durations are inclusive, completion-charged wall time. They include descheduling,
blocking and nested work, and overlap across threads. They cannot be summed to
frame time, called import-body CPU, or converted to CPU by subtracting a trivial
call cost. In particular the condvar totals do not alone establish how much CPU
main performs or which producer satisfies every wakeup.

Build confirmed the full-sync pair has not run yet: the previous artifact
stopped in the clinic, and gameplay currently owns the GPU slots. The pair is
queued for shared 33a8af0 at the next orchestrated quiet slot. Keep its matched
no-summary control and append exact TLS/time names, because `sync` does not
select these:

```sh
BB_HLE_TRACE_SUMMARY=60
BB_HLE_TRACE_SUMMARY_FILTER=sync,gnm,videoout,avplayer,scePthreadSelf,scePthreadGetspecific,__error,__tls_get_addr,sceKernelGetProcessTime,sceKernelReadTsc
```

This is still a selected-import census, not all imports. Report counts separately
for main and GX; do not merge worker-thread activity into them. Multiply a
measured fast-path CPU cost only by calls with the corresponding path/arguments,
or clearly label it a hypothetical estimate. A contended lock's total wall time
is not its fast-path CPU cost.

## Optimization decisions

Pending counts, the likely inexpensive candidates are clock reads, cached TLS
lookups, and uncontended mutex operations. Their actual implementations already
have cached fast paths: `runtime_thread_current` returns TLS `current` when set;
`__tls_get_addr` allocates only the first time per module/thread; `key_get` indexes
thread-local storage. Clock functions call Darwin clock APIs; mutex success paths
include relaxed global diagnostic-counter increments. These are inspectable
body costs, not a generic transition tax.

Do not inline guest clock/TLS/synchronization code or remove synchronization on
the basis of a synthetic call benchmark. First identify real counts and CPU
cost, then preserve clock semantics, TLS initialization, errors and mutex
ownership. The currently observed large inclusive wall durations are condvar handoff and
submit waits. They do not establish a CPU-cost ranking or rule out expensive
unobserved imports.
