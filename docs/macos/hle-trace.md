# Filtered HLE import timeline

`BB_HLE_TRACE=sync,gnm,videoout,avplayer` wraps selected resolved host function
imports at `runtime_resolve`. Data imports and linked native guest exports are
not wrapped. Unset/empty/`0` returns the original pointer; there is no per-call
branch or trampoline when disabled. This is an opt-in diagnostic, not a timing-
neutral observation.

Filters may be comma-separated categories or exact symbol/scoped-NID names:

- `sync`: mutex, condvar, semaphore, event flag, equeue, usleep/nanosleep, yield,
  and clock-gettime calls.
- `gnm`: Gnm names containing Submit, Wait, Flip or Label. Builders are excluded
  to retain the handoff timeline. Select a builder by its exact symbol if needed.
- `videoout`: all sceVideoOut imports.
- `avplayer`: all sceAvPlayer imports.

Each ENTER/EXIT record contains a global sequence, paired call ID, raw TSC,
pthread identity/name, import symbol, original caller, and first four integer
argument registers. EXIT adds RAX/RDX return bits, elapsed TSC ticks, and the
number of intervening traced frames abandoned by nonlocal control transfer.
Integer fields are hexadecimal. Duration includes tracing overhead and is not a
CPU-time measurement. Floating argument/result bits are preserved by the bridge
but are not decoded into these four integer-argument fields.

The ring retains 8192 records (roughly 4096 complete calls, less if calls remain
pending); overwritten records are discarded oldest first. `runtime_report` dumps
it. The exported `runtime_hle_trace_dump` is also the parser-stop integration
point. `BB_HLE_TRACE_STREAM=1` additionally writes records as calls run, at greater
cost. Concurrent streamed lines may appear out of sequence; use the sequence
field. Normal imports use a short global atomic gate to publish fixed records.
A fatal dump attempts that gate once: if interrupted during publication or
another dump, it prints an unavailable marker rather than spinning or reading
an incomplete record. Dump formatting uses fixed stack buffers and write(2),
without TLS lookup, allocation, name lookup or pthread locks.

The x86-64 bridge preserves stack arguments, AL, integer registers, flags, x87,
XMM and YMM state around logging. Host errno and guest TLS errno are preserved.
It changes the function's return PC, so older diagnostics using
`__builtin_return_address` inside a traced HLE function see the bridge; the ring
contains the original caller. Target stack arguments are not copied. Callers
cannot retain red-zone values across a function call under the SysV ABI.

Per-thread bookkeeping is bounded to 1024 calls and keys them by their original
return slot. On return it discards skipped inner frames; repeated calls at a
reused slot replace stale records. This handles a callback longjmp back into an
outer traced function, followed by that outer function returning. Calls abandoned
without a matching outer return can retain entries until slot reuse; exhausting
the bound stops loudly. There is no C++ exception unwind metadata for these
bridges: exceptions unwinding across a traced import are not qualified. Do not
use this diagnostic to claim normal exception/unwind behavior.

`tests/test_hle_trace_macos.sh` is wired into Darwin `--test`. The fixture covers
disabled pointer identity and filtering, ten integer arguments (including stack
arguments), variadic FP, x87 long-double and aggregate returns, YMM arguments and
return, nested callbacks, repeated inner longjmp with outer return, both errno
domains, concurrent ring wrap, and a signal/fault interrupting publication on the
publishing thread. Logs from a busy fatal dump explicitly state their limitation.

## Aggregate wait attribution

`BB_HLE_TRACE_SUMMARY=600` reports every 600 actual VideoOut flips and once at
ordinary `runtime_report`. With `BB_HLE_TRACE` unset, summary mode does not publish
or print ring records. `BB_HLE_TRACE_SUMMARY_FILTER` accepts the same filter syntax;
its default is `sync,gnm,videoout,avplayer`. Setting `BB_HLE_TRACE` as well enables
both modes with that variable's filter, at the detailed trace's additional cost.
Use a smaller positive frame interval when frames are slow. `0` disables summary.

Each pthread has its own cached identity and counter table. Calls take a short
per-thread gate for bookkeeping, with no global per-call publication, formatting
or name lookup. Reporting copies/resets each table under its gate, then releases
it before writing stderr. A frame/report mutex serializes only reports and frame
counting. Counter nodes persist until process exit so terminated threads remain
reportable; names are cached at their first selected call, not updated on rename.
The existing ABI bridges, nonlocal-exit handling and exception limitations apply.

Output names the interval wall duration, flips, pthread/name, import, completed
calls, `call_wall_ms`, `max_ms`, `wall_pct`, abandoned calls, and in-flight calls.
A separate active-import line gives the innermost pending call's age, including a
wait that has not returned. Abandoned calls are counted when slot reuse or an
outer return proves their abandonment; their durations are not guessed.

These are **inclusive call wall durations, not measured blocked CPU time**. An
HLE can compute, block, or call guest code during that interval. Nested import
values are non-additive. The whole duration is charged to the reporting interval
in which the call completes, so a call spanning windows can yield over 100%.
Pending time is shown separately and must not be added to completed time. Table
snapshots occur sequentially, rather than freezing every thread at one instant.
Use the CPU profile alongside these numbers to distinguish execution from waits.
There is no signal-handler summary: the fatal ring dump remains nonblocking and
summary-only mode leaves that ring disabled. A hard stop before the next flip
report can therefore omit the final partial interval.

Existing host wait counters (separate from guest import timing):

| Site | Existing measurement / missing attribution |
| --- | --- |
| Scheduler::WaitDeferredSignals | `deferred_signals_wait_ns` |
| Scheduler::WaitHostCopies | `host_copy_record_wait_ns`, then `copy_threads_wait_ns` |
| Scheduler::SyncRecording | `sync_recording_ns` |
| Scheduler::Wait(tick) | `tick_wait_ns` |
| DrawPipe::Drain | TSC duration/count by drain reason |
| Liverpool submit_cv / RecorderThread recorder_cv | Add idle-wait count/total/max around predicate waits; distinguish idle from dependency waits |
| DrawPipe::Run wake.wait / WaitForSpace | Add empty-queue idle and full-queue backpressure separately |
| Scheduler deferred-op worker | Separate its queue idle wait from GPU timeline wait |

The first five counters already feed frame/BB_PERF reports. They aggregate call
sites rather than distinguishing all calling threads, and nested wait timers
must not be summed blindly. No host-wait behavior changes are part of this unit.

The existing signed fixture also runs in summary-only mode: ABI/errno controls,
nonlocal abandoned calls, four concurrent threads with exact independent counts,
a held wait reported before completion, duration/max after release, interval
reset and frame cadence. The fixture's optional `bench off` / `bench on` modes
measure one million trivial imports; compare CPU ns/call as well as wall time.
This establishes instrumentation cost, not a gameplay FPS claim. Narrow the
filter for steady performance runs if high-frequency mutex/clock calls dominate.

Qualification for 905d516 on this Rosetta host: three paired one-million-call
runs measured off 2.84/2.61/3.40 ns CPU per trivial call and summary-on
341.17/342.90/343.53 ns (`getrusage(RUSAGE_SELF)`, user + system). Summary-on wall
cost was 3.85–4.96 microseconds per call under contention, so it is not a quiet
throughput result. At 100,000 selected calls/second, the measured CPU increment
would be about 34 ms/second; count-dependent overhead still needs a gameplay A/B.
`CLOCK_THREAD_CPUTIME_ID` produced implausibly small values in an earlier probe
and was not used for these numbers. Evidence: `out/hle-trace-summary-integration.log`.
The affected VideoOut driver compiled with production flags; the signed fixture
passed both detailed and summary modes. No shared artifact was rebuilt.
