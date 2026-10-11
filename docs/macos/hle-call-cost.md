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
library. `BB_HLE_COST_SOURCE_ROOT` defaults to this checkout and supplies the
LibAtrac9 submodule headers when they are populated in another checkout. It does not
launch the game or initialize the GPU renderer. Run through the quick pool:

```sh
BB_HLE_COST_GPU=/absolute/path/to/pinned/libbbgpu.dylib \
BB_HLE_COST_ATRAC=/absolute/path/to/libatrac9.a \
BB_HLE_COST_SOURCE_ROOT=/checkout/with/third_party/headers \
  tools/macos/heavy.sh --pool quick hle-cost -- \
  bash tools/macos/hle_cost/run.sh
```

A generated low-address executable stub uses the resolved guest PLT instruction
`jmp qword [rip+displacement]` through a GOT target. The trivial direct and PLT
controls use the same noinline callee and loop. Other rows resolve real NIDs once
and call the actual exported HLE function, including its error-wrapper checks and
bookkeeping. TLS and object allocation are warmed before measurement. No summary,
ring trace or CPU sampling is enabled.

Each row has one million iterations in five passes, with order reversed in
alternate passes. CPU is process user+system time from `getrusage`; elapsed time
is measured separately with `mach_absolute_time`. CPU values include the loop,
indirect call, return and a volatile result store. The full trivial PLT cost is a
conservative bookkeeping estimate for a hot import boundary, not an isolated
instruction latency or a bound on cold/mispredicted calls. Its difference from
the direct control estimates the additional guest PLT jump on this hot workload.
Mutex lock/unlock and semaphore wait/signal rows are **pairs**; the tool never
pretends to separate their body costs. Condition signals have no waiters and
semaphore waits already have a token. Those are fast-path controls, not models of
contended game waits.

Results are not game CPU measurements. Scheduling, CPU migration and Rosetta's
translation/cache behavior can differ in gameplay. Wall times on the loaded
machine especially cannot be substituted for per-call CPU cost.

## Measured CPU cost

Signed quick-pool run `out/hle-cost/run.GBm8JY` completed all five passes on
2026-10-11. The production runtime files used here are byte-identical to their
2a874fd versions. The output directory retains CSV, raw stdout/stderr, copied
benchmark sources, source revision and pinned library hash. One-minute host load
was 221–236, so these are loaded-host process-CPU measurements, not a quiet FPS
qualification. CPU samples were much more stable than elapsed samples.

| Case | Median CPU ns/iteration | Min–max CPU ns | Calls/iteration |
| --- | ---: | ---: | ---: |
| Trivial direct control | 2.974 | 2.947–3.008 | 1 |
| Trivial guest PLT control | 6.707 | 6.319–6.835 | 1 |
| sceKernelClockGettime, monotonic (4) | 55.133 | 54.507–55.330 | 1 |
| sceKernelClockGettime, realtime (0) | 47.150 | 47.022–47.765 | 1 |
| sceKernelGetProcessTime | 54.723 | 54.495–55.856 | 1 |
| sceKernelReadTsc | 13.575 | 12.471–14.752 | 1 |
| sceKernelWaitSema + SignalSema, token available | 36.603 | 35.191–38.034 | 2 |
| sceHttpWaitRequest, zero timeout | 4.819 | 4.519–5.091 | 1 |
| scePthreadSelf, attached thread | 14.677 | 14.278–15.242 | 1 |
| scePthreadGetspecific | 13.020 | 12.612–13.623 | 1 |
| __error | 14.004 | 13.810–14.235 | 1 |
| __tls_get_addr, allocated module | 13.066 | 12.751–13.331 | 1 |
| scePthreadMutexLock + Unlock, uncontended | 38.466 | 38.197–38.633 | 2 |
| scePthreadCondSignal, no waiters | 12.543 | 11.599–13.261 | 1 |
| scePthreadCondBroadcast, no waiters | 12.702 | 12.501–12.976 | 1 |

The extra PLT jump measured 3.733 ns versus the direct control; the full 6.707 ns
control is the conservative attribution proxy used below. Different callee code
and branch layout matter: the trivial zero-timeout HTTP body is cheaper than
that control, so subtracting 6.707 ns from every measured function would not
produce a reliable isolated body cost. The table therefore reports body-plus-call
CPU cost directly, with a separate trivial boundary proxy.

For scale, 1,000 monotonic-clock calls cost about 0.055 ms CPU, 1,000 cached TLS
lookups about 0.013 ms, and 1,000 mutex lock/unlock pairs about 0.038 ms in this
microbenchmark. It would take about 18,138 monotonic-clock calls or 25,997 mutex
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
  --windows 3 --boundary-ns MEASURED_FULL_TRIVIAL_PLT_NS
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

Main contributes 35.056 observed calls/flip and GX 15.200. Multiplying by the
full trivial PLT control gives **0.000235 ms CPU/flip on main** and
**0.000102 ms on GX**, or 0.000337 ms combined. At 0.357 microsecond added tracing
CPU per call, estimated summary bookkeeping is 0.012515 / 0.005426 ms respectively.
These cover only selected completed calls and exclude report I/O and effects on
scheduling. They do not account for the omitted mutex/clock/TLS traffic.

The only measured fast-path body in these rows with potentially comparable
arguments is zero-timeout `sceHttpWaitRequest`: its 1.994 calls/flip would cost
0.000010 ms CPU at the measured 4.819 ns rate **if** the game's calls use zero
timeout. The aggregate does not record arguments, so that condition is not
established. Neither condvar blocking nor real GPU submissions can be modeled
by the trivial or uncontended controls.

Durations are inclusive, completion-charged wall time. They include descheduling,
blocking and nested work, and overlap across threads. They cannot be summed to
frame time, called import-body CPU, or converted to CPU by subtracting a trivial
call cost. In particular the condvar totals do not alone establish how much CPU
main performs or which producer satisfies every wakeup.

For future full-sync data, keep the paired no-summary control and append exact
TLS names, because `sync` does not select them:

```sh
BB_HLE_TRACE_SUMMARY=60
BB_HLE_TRACE_SUMMARY_FILTER=sync,gnm,videoout,avplayer,scePthreadSelf,scePthreadGetspecific,__error,__tls_get_addr
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
ownership. The currently observed large costs remain condvar handoff and submit
waiting, rather than frequent import crossings.
