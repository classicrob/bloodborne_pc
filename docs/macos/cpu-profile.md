# In-process CPU profiles under Rosetta

`BB_CPU_PROFILE` enables an opt-in x86_64 Darwin sampling profiler. It does not
attach a debugger, suspend another thread, or request another thread's registers.
It is a diagnostic, with measurable overhead. Keep ordinary FPS baselines
separate from profiled runs.

## Capture and read

Pass these variables through the normal game launcher, in a Playtest/heavy-job
slot. The output file must not already exist; its parent directory must exist.

```sh
BB_CPU_PROFILE=/absolute/local/path/game-cpu.jsonl
BB_CPU_PROFILE_HZ=1000
BB_CPU_PROFILE_WINDOW=120:140
```

The window is wall-clock seconds after profiler initialization, not game frames
or time since entering the world. Without a window it runs until normal exit,
`runtime_report()`, or `kill -USR1 <bb-probe-pid>`. SIGUSR1 stops sampling and
finishes the dump; it does not terminate the game. There is no restart operation.
SIGUSR2 remains the existing runtime diagnostic. A conflicting SIGPROF, SIGUSR1
handler or existing ITIMER_PROF is a loud configuration error.

```sh
python3 tools/macos/cpu_profile.py game-cpu.jsonl \
    --link-json /path/to/link.json --top 20 --folded game-cpu.folded
```

The reader prints per-thread flat counts, the guest / HLE-runtime / GPU-library /
MoltenVK / other split, and optional folded stacks for a flame graph. Host
symbolization uses offline `atos`, after checking the captured Mach-O UUID against
the file currently on disk. Preserve the matching executable, libraries, object
files or dSYMs alongside a capture. Missing or mismatched images retain image
names and offsets with an explicit warning; the tool never silently symbolizes
against a different build. System shared-cache images may also be offsets-only.
Use `--no-symbols` to avoid external symbolizer calls.

Guest addresses are `eboot+offset` or `prx[index]+offset`; `--link-json` supplies
PRX filenames from `scripts/link_modules.py`. Return PCs are decremented by one
only for symbol lookup, so the call instruction belongs to its caller. The leaf
uses the exact interrupted RIP. The category split uses the leaf: `bb-probe` is
HLE-runtime, `libbbgpu` is GPU-library, MoltenVK images (or resolved MVK/mvk
symbols in a static image) are MoltenVK. Unresolved static library code cannot
be split more finely than its containing image. These are CPU samples, not GPU
time or time waiting for the GPU.

## Signal and publication design

ITIMER_PROF is a **process CPU timer**, requested at 1–4000 Hz (default 1000).
Darwin's XNU `bsd_ast` charges the current thread's CPU-time delta and first tries
to deliver SIGPROF to that thread (`psignal_try_thread`). A fixture with two busy
workers and a sleeping worker observed main=1, busy workers=257/225, sleeper=0.
This supports sampled CPU attribution on the qualified Mac. It does not create
independent per-thread 1 kHz clocks. Timer granularity, coalescing, signal masks,
redirection to an eligible thread and scheduling change the effective rate.
Do not convert one sample to exactly one millisecond or compare raw counts from
different windows without considering total samples.

Primary implementation references:
[XNU signal delivery](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/kern/kern_sig.c)
and [CPU-time accounting](https://github.com/apple-oss-distributions/xnu/blob/main/osfmk/kern/task.c).
External/user-generated SIGPROF also produces samples; do not mix it with another
profiler. A thread with SIGPROF blocked is not fully observed.

The SIGPROF handler uses the interrupted context, a preallocated per-thread ring,
lock-free atomics, cached name/bounds, `pthread_self`, `mach_absolute_time`, and
bounded direct stack loads. It performs no symbolization, allocation, name or
stack-bound query, Mach memory read, or output. Both helper bindings are exercised
before the timer starts. This is qualified for Darwin/Rosetta, not a claim of
portable POSIX signal safety for every platform.

Known runtime threads register at naming/attachment and stack switching; GPU
threads register through `Common::SetCurrentThreadName`. PipelineBuild and
PipelinePreload worker naming is a separate renderer unit (355d870). Unregistered
third-party threads get RIP-only samples, explicitly flagged and counted in the
report. Names are copied into each sample; thread retirement clears cached bounds
and names before pthread storage can be reused. A reused pthread address with the
same name is aggregated in the report, not a distinct lifetime identifier.

At most 12 PCs are stored. A frame must be aligned, fully inside a cached native
or low-stack range, and advance the frame pointer. Missing/omitted frame pointers
truncate or make the chain unreliable; the leaf RIP remains the interrupted PC.
Cached bounds alone are not assumed safe: the two assembly load instructions
have an exact-PC SIGBUS/SIGSEGV recovery, permitted only while this thread's
profiler unwind is active. Both words must load before publishing a frame. An
unreadable frame ends the walk and flags the sample. All other faults follow the
existing entry-probe, GPU tracker and fatal paths unchanged.

Each ring has one producer (that thread's SIGPROF handler, non-reentrant for
SIGPROF) and one collector. Release/acquire publication prevents the collector
from reading a partial sample or the handler from overwriting an unread one.
Rings have 512 records per pthread and 256 slots (about 25 MiB); slots are not
reclaimed during a capture. Overflow is counted, not silently overwritten.
The collector drains about every 20 ms and flushes JSONL. A fatal `_exit` retains
previous flushes, but can lose the last interval and has no final summary. An
incomplete last line is explicitly ignored by the reader. Normal stop disables
the timer, quiesces active samplers, drains and reports drops/read failures.
Image add/remove metadata is published outside signal handlers; its independent
512-event cap is reported on overflow. The collector blocks SIGPROF; process
CPU accounting and redirection mean its cost is not perfectly separable in the
profile itself.

## Qualification

Run through the short-job pool:

```sh
tools/macos/heavy.sh --pool quick cpu-profile -- sh tests/test_cpu_profile_macos.sh
```

The script is also wired into Darwin `build.sh --test`. Signed SDK-only fixtures
cover default-off behavior; two known hot functions on different threads; name
retirement and uncached RIP-only threads; deliberate ring overflow with counted
drops; window and SIGUSR1 finish; offline symbols and folded stacks;
protected first/second frame words and unmapped stale bounds; overflow-safe frame
bounds; unrelated faults while unwind-active; 1000 write-protection faults;
a blocked pipe read under SIGPROF; and the entry probe's 34 concurrent hits with
GPR, flags, YMM and stack preservation while SIGPROF interrupts SIGTRAP handling.
The real runtime sleep and semaphore code is used for interrupted-wait controls.
SA_RESTART restarts the pipe read; sleep wrappers explicitly retry EINTR; semaphore
condition waits retain their predicate/timeout semantics. This is not a blanket
claim that every third-party blocking API is EINTR-safe.

Six alternating fixed-work baseline/profile pairs at 1 kHz on macOS 27.2 under
Rosetta measured process CPU ratios 1.0685, 1.0700, 1.0704, 1.0561, 1.0583, 1.0624:
median **1.0654 (6.5% CPU overhead)**. Baseline process CPU was 0.670–0.677 s;
profiled 0.713–0.720 s, with 485–522 samples and zero drops per run. This includes
handler and collector CPU in that workload. The host was heavily contended:
wall-time ratios ranged 0.951–1.486, so those are not an FPS or latency estimate.
A quiet gameplay measurement must establish the cost for its actual thread mix
and stack depths. Raw local evidence: `out/cpu-profile/bench-1791695015495094000/`.
