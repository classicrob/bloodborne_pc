# Condition-wait wake attribution and possible filtering

Source audit, 2026-10-11. This is a design note, not a production change or a claim that the
movie collision is caused by spurious wakes. PM4 owns the generation-counter diagnostic;
Astra-2 reviews its runtime integration. Game qualification is separate.

## Evidence and limits

The engine event wait at image offset `0x207e8f0` checks event-state bit 1 under its mutex,
then calls the condition helper `0x2083e80` once. It returns the helper result without checking
the predicate again. The helper maps a successful condition wake to 0 and timeout to -3.
Our `runtime_mutex.c` forwards condition waits to host pthreads.

Apple's published psynch path maps EINTR to success, reacquiring the mutex before return;
its ulock path also permits that outcome. This establishes a possible mechanism, not that it
occurred on the installed library in the failing game run. See
[Apple libpthread, pinned source](https://github.com/apple-oss-distributions/libpthread/blob/42d026df5b07825070f60134b980a1ec2552dfee/src/pthread_cond.c#L587).

FreeBSD 9 has different paths: `cond_wait_user` keeps waiting while its waiter remains queued,
whereas `cond_wait_kernel` maps EINTR to success. Scheduling policy, mutex attributes and
process sharing select the path. This is useful precedent for a waiter queue, but neither a
universal no-spurious guarantee nor proof of Sony's implementation. See
[FreeBSD 9 libthr, pinned source](https://github.com/freebsd/freebsd-src/blob/0d9d75b124880ee5a3aeb5c2af92a04340c02bef/lib/libthr/thread/thr_cond.c#L148).

POSIX permits spurious returns, does not expose EINTR from these functions, and requires
mutex reacquisition before cancellation cleanup. A cancellation must not consume a signal
needed by another blocked waiter. See
[condition-wait specification](https://pubs.opengroup.org/onlinepubs/7908799/xsh/pthread_cond_wait.html).

KICK2 supports main-only entry to the kick on its observed route. Its arguments are a=1,
b=0, job nonnull. Mode zero waits synchronously for done; nonzero mode returns without
kicking. The actual mode and the exact successful wait with a false predicate remain the
important evidence. A signal delivered to some other condition does not explain that wait.

## Review requirements for the diagnostic

Keep host wait behavior unchanged. Track condition instance identity, original guest caller
(including traced imports), thread, wait result, and notification sequences before/after.
Both sce and POSIX exports must share accounting. Preserve errno and native-error reporting.

Publish notification intent before native signal/broadcast, otherwise a fast waiter can
return before the diagnostic records it. Record notification failures separately: attempted
notification is not proof of delivery. Use atomic publication and explicit ambiguity when
attempt completion races the snapshot. Never take the guest mutex from signal/broadcast or
hold a diagnostic lock across a blocking wait. Retire enrolled records on cancellation.

An unchanged sequence with successful return is an unnotified-return candidate, provided all
notification paths and the condition lifetime were observed. A changed sequence cannot prove
this waiter was selected: one signal may overlap many waiters, and notifications can occur
between wrapper enrollment and native sleep or during mutex reacquisition. A generation
counter alone must not become a success predicate for a general implementation.

Controls: real signal, broadcast, a direct native wake bypassing notification instrumentation,
timeout, simultaneous waiters, a signal at enrollment, cancellation, destroy/reinit address
reuse, and traced/untraced caller identity. Capturing the engine done predicate under its own
mutex adds evidence specific to GX; do not infer that predicate from notification counts.

## If filtering is justified by target evidence

Use an explicit per-condition waiter queue rather than `while (generation == old)` around
the host wait. The following is a proposed protocol requiring fixtures and review:

1. While holding the caller's guest mutex, acquire the condition's internal queue lock and
   enroll a waiter with a unique ticket. Release the guest mutex while still holding the
   queue lock. Sleep on a private host condition using the queue lock. This closes the
   enrollment/unlock gap without requiring signalers to acquire the guest mutex.
2. Signal selects one eligible enrolled waiter and grants it a notification; broadcast grants
   all currently enrolled waiters. Notifications with no eligible waiter are discarded, not
   stored for future arrivals. A private wake per waiter avoids waking an unselected waiter
   while leaving the selected one asleep. Host spurious wakes recheck the grant under the
   queue lock.
3. Resolve notification versus timeout under that same lock. Remove the waiter, release the
   queue lock, and reacquire the guest mutex before ordinary return. Never wait to acquire
   the guest mutex while holding the queue lock. Preserve native mutex errors and the
   existing sce/POSIX error mappings.
4. Compute one deadline for a relative microsecond call; repeated host wakes must not extend
   it. Preserve the absolute clock domain for POSIX timed waits. A signal/timeout race needs
   an explicit target-qualified policy; do not silently convert every raced timeout to success.
5. Install cancellation cleanup before becoming cancelable in the wait. Host cancellation
   reacquires the queue lock; cleanup removes enrollment, releases that lock and reacquires
   the guest mutex before guest cleanup runs. If a canceled waiter was awarded a single
   signal, preserve that notification for another eligible waiter from the same enrollment
   epoch, not a later arrival. Cancellation at grant, timeout and mutex-reacquisition boundaries
   needs dedicated controls. Do not convert cancellation into a successful ordinary return.

Lifetime pinning, destroy-with-waiters behavior, mutex type/recursive-depth handling and guest
thread exit need explicit decisions before this replaces existing waits. The current runtime
has no complete guest cancellation interface; that is not permission to break host pthread
cancellation used by fixtures or future callers.

The minimum acceptance suite also checks: one signal cannot release two waiters; broadcast
cannot release a later arrival; real notifications are not lost on cancellation; recursive and
error-check mutex behavior; unchanged timeout under repeated injected wakes; and caller mutex
ownership in every return/cleanup path. Until target semantics and those controls are settled,
keep any filtering opt-in and retain the narrower movie predicate-loop experiment.
