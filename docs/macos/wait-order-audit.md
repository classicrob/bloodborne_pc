# Movie handoff: early-wait audit

Static audit of the macOS runtime at `f87e370`, 2026-10-10. This does not establish
the cause of the copied graphics contexts or qualify PS4 behavior on hardware.
The entry probe's time-ordered copy/init/destruction records take priority over
the hypotheses below. No game or synchronization test was launched for this audit.

## Result and priorities

No ordinary clock-unit mismatch or timeout-as-success path was found in the
condition-variable and semaphore waits used by the game. There is a concrete
unlocked event-queue poll path, and some separate unsupported semantics. None
has yet been connected to the movie's parent/child context handoff.

1. `EqueueInternal::WaitForEvents` calls `GetTriggeredEvents` without `m_mutex`
   when the timeout pointer contains zero. The helper reads, clears and erases
   elements of `m_events`. Triggering, registration and blocking waits protect
   those same objects with `m_mutex`. Two polls can race consumption of one
   clear event, and polling can race vector mutation. This is C++ data-race UB,
   not proof of a particular duplicated flip notification. Renderer ownership
   has been notified. A fixture should first hold the queue mutex, start a poll
   on a pre-triggered clear event, and show that polling cannot consume it until
   the lock is released; then test two pollers consuming exactly one event.
2. `runtime_kernel.c` discards non-EINTR errors returned by `sleep_ns`, including
   in `nanosleep`, and returns success. Its seconds-to-nanoseconds multiplication
   can overflow for enormous, otherwise nonnegative timespecs. These are real
   source-level error-path issues, but not evidence of early movie handoff:
   ordinary 32-bit microsecond sleeps do not overflow and use valid timespecs.
   A fixture should inject a failing native nanosleep and inspect the resolved
   guest API's result/errno; a separate intercepted-call control should inspect
   a large timespec without actually sleeping for centuries.
3. Semaphore priority ordering, non-default condvar attributes, and kernel
   EventFlag behavior are not fully implemented. They must not be described as
   exact PS4 implementations. See the limits below and the import census.

No behavior changes or speculative delays are part of this audit.

Renderer review agrees the submission-lock store below is a formal data race,
but ranks it below the context-copy probe: on Apple, GpuIdle is emitted with
`submit_mutex` held and no queued submissions, after draining and label writes.
A late true store is followed by the SubmitDone-triggered idle check; the owner
found no logical early-release path from this race alone. See
[gpu-idle.md](gpu-idle.md).

## Wait-by-wait trace

| Surface | Current implementation | Early-success assessment / limit |
| --- | --- | --- |
| `scePthreadCondTimedwait` | Relative `uint32_t` microseconds are added to `CLOCK_REALTIME`; the native condvar uses its default realtime clock. Nanoseconds use `usec * 1000` after splitting seconds. | No realtime/monotonic mismatch in the normal path. ETIMEDOUT becomes `0x8002003c`, not zero. A wall-clock jump can affect this relative-to-realtime conversion; exact console behavior has not been measured. |
| POSIX `pthread_cond_timedwait` | Validated guest absolute seconds/nanoseconds are passed to native realtime timedwait. POSIX wrapper returns positive guest error 60 for timeout. | No unit conversion or unconditional success. `cond_init` ignores a non-default attribute pointer, however; that is not qualified for a caller selecting another clock. |
| Condition signal / broadcast | Native `pthread_cond_signal` / `pthread_cond_broadcast`, respectively. | Not interchanged. A successful condition wait is not itself proof that a guest predicate became true. |
| `sceKernelWaitSema` | Relative microseconds become a monotonic deadline. On Apple each retry converts remaining monotonic time to `pthread_cond_timedwait_relative_np`. The waiter loops until its own `done` bit is set under the registry mutex. | Spurious native wakes do not grant a token. Timeout removes the waiter and returns `0x8002003c`; cancel returns `0x80020055`; delete returns `0x8002000d`. These errors are not converted to success. |
| `sceKernelPollSema` | Takes tokens only when count is sufficient; otherwise returns `0x80020010`. | No successful empty poll. A new waiter can bypass an older waiter needing more tokens; exact general ordering is not qualified. |
| Semaphore cancel / signal | Removal, result and notification occur under the same registry mutex. Signaling removes a granted waiter before waking it; cancel cannot grant it a second token. | The previously qualified need=1/max=1 heap lock has no identified duplicate-token path. Priority attr 2 is implemented as FIFO, not PS4 priority order. Overflow currently returns EINVAL; exact console overflow errno remains unqualified. |
| Equeue blocking / timed wait | Predicate consumes triggered events while holding `m_mutex`; timeout duration is microseconds. `sceKernelWaitEqueue` returns ETIMEDOUT if the resulting count is zero. | Spurious wakes cannot return a successful zero-event result. Zero-timeout polling has the race above. The global `kqueues` map also lacks synchronization/lifetime ownership across concurrent create/delete/lookup. |
| Equeue small timers | Steady-clock deadline; spins until an expired small timer is found or caller timeout expires. | Not an early-success conversion. This path ignores ordinary triggered events while small timers are present, a separate possible late-wait/hang issue. |
| `usleep`, `sleep`, `nanosleep` | Normal units are microseconds, seconds and timespec respectively. EINTR is retried until the requested duration completes. | Retrying EINTR can wait longer than POSIX interruption semantics, not shorter. Other native errors are incorrectly hidden; large nanosleep input can wrap as above. |
| AvPlayer `EventCV` | Predicate overloads of condition-variable wait, stop-token wait and timed wait. Notifications acquire the CV mutex after queue updates release the queue mutex. | Native spurious wakes are filtered. Stop-token waits return a bool which callers must honor. The fixed queue/Stop lifecycle does not change callback thread identity. |
| `sceVideoOutWaitVblank` | Holds `vo_mutex`, snapshots the count, waits for `count != previous`. | No success from a spurious CV wake alone. Whether the producer increments at the right emulated point belongs to the VideoOut/PM4 ordering audit. |
| Gnm `WaitGpuIdle` | Waits with predicate `submission_lock == 0`; `ResetSubmissionLock` clears it and broadcasts under `m_wait_idle`. | Predicate filters spurious CV wakes. It is a submission-lock wait, not independently a GPU-completion proof. `sceGnmSubmitDone` sets the plain lock variable outside `m_wait_idle` (under `m_submit_lock`), while the interrupt reset uses `m_wait_idle`; this deserves a separate ownership/race check by the Gnm owner. |

## What the game imports

Read-only parsing of the plaintext eboot and all seven `sce_module/*.prx`
dynamic symbol tables, matching the selected names by the repository's NID
algorithm, found:

- Eboot: both POSIX and SCE condvar waits/timed waits, semaphore wait/poll,
  `sceKernelWaitEqueue`, and `sceKernelUsleep`.
- Fios2: SCE condvar wait/timedwait and `sceKernelUsleep`.
- libc: SCE condvar wait/timedwait and `nanosleep`.
- HeadTracker: `sceKernelWaitEqueue`.
- The other four PRXs: none of these selected wait imports.

None of these modules statically imports the selected `sceKernelCreateEventFlag`,
`sceKernelWaitEventFlag`, `sceKernelPollEventFlag`, POSIX condattr-setclock or SCE
condattr-setclock names. This excludes neither dynamically resolved symbols nor
inline code. There is no EventFlag wait implementation in this runtime/GPU shim;
an unresolved call is a loud unsupported import, not a fake successful AND/OR
wait. Consequently AND/OR, CLEAR_ALL/CLEAR_PAT and multiple-waiter semantics
cannot be claimed to work here, but they are not a supported wait secretly
returning success on this route.

## Reference boundary

[FreeBSD 9 libthr's condition implementation](https://raw.githubusercontent.com/freebsd/freebsd-src/releng/9.0/lib/libthr/thread/thr_cond.c)
defaults to realtime and accepts absolute deadlines. Its ordinary userspace
wait queue loops until notification or timeout, whereas its kernel path can
convert interruption to a successful return. It is a useful ancestry reference,
not a measurement of Sony's libkernel. The host implementation may have a
different frequency of spurious wakeups.

[POSIX condition-variable semantics](https://pubs.opengroup.org/onlinepubs/009604599/functions/pthread_cond_timedwait.html)
permit spurious success and require rechecking the protected predicate. A guest
that relies on a stronger implementation-specific guarantee needs direct
call-site evidence before we replace the native condvar with another wait
implementation. Adding a delay or suppressing all wakeups until an invented
predicate would not reproduce PS4 semantics.

The existing signed heap-lock fixture and its limits are recorded in
[guest-heap-lock.md](guest-heap-lock.md). The static audit adds no new runtime
qualification to those results.
