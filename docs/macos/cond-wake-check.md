# Unnotified condition wakes (`BB_COND_WAKE_CHECK=1`, diagnostic)

Guest condition waits (`scePthreadCondWait`/`TimedWait`, `pthread_cond_wait`/`timedwait`) run
on host `pthread_cond_*` (src/runtime_mutex.c). On Darwin, a wait whose psynch/ulock call is
interrupted (by a signal, for example) returns success without a wake. The port interrupts guest
threads with signals: write faults, the CPU profiler and entry probes. Guest code that waits once
without rechecking its predicate then goes on early. The engine's event wait 0x207e8f0 does
exactly that (docs/macos/movie-handoff.md).

With `BB_COND_WAKE_CHECK=1` (macOS), each cond counts notifications (signal and broadcast).
Each is counted **started** before its native wake and **completed** after it. A wait takes both
counts before it waits (completed first) and classifies a successful return:
- **notified**: a notification started while it waited. This is no proof that this waiter was
  the one woken, since one signal can overlap several waiters, so it undercounts candidates.
- **ambiguous**: none started while it waited, but one was in progress when it began (started
  but not completed). That notification may be the one that woke it.
- **unnotified wake candidate**: neither. No notification could have woken it, because one that
  woke it would have been started before its native wake, either while it waited or in
  progress when it began. The paused-signaler control checks exactly this.

The counts live in a per-cond record that is never freed. POSIX lets a thread destroy a cond once
its waiters are awake, while they are still reacquiring the mutex, so a returning waiter reads the
record it took before waiting, never the cond. Each record has a unique id; a cond's address can
be reused.

Waits behave the same with the check on or off. Off, no record is made. The check:
- enrolls nothing per waiter, so a cancelled wait leaves nothing behind;
- holds no diagnostic lock across a wait;
- never takes the guest mutex in signal or broadcast;
- keeps errno and the guest errno around its own work;
- disables cancellation while it holds its own lock to report, so a cancelled reporter cannot
  leave that lock held.

The records are kept for the life of the process (per-cond growth while the check is on). Sites
are keyed by pthread identity; a pthread_t reused after a thread exits merges the two histories
when the caller and cond also match.

Native signal and broadcast failures are counted separately.

## Output

The output goes to stdout.
- `COND-UNNOTIFIED candidate #n: thread "<name>" (pthread <id>) caller <guest return address>
  cond #<id> ...` is printed for the first 32 candidates.
  - The caller is the guest's return address from the sync import. It comes from the HLE
    trace's record when `BB_HLE_TRACE` or its summary replaced the return slot.
  - The cond id is unique per cond created, unlike its recyclable address.
- `COND-UNNOTIFIED summary` gives the counts of candidates and ambiguous wakes, and the count
  per site: thread (pthread identity and name), caller and cond. 64 sites are tracked; the
  candidates at further sites are counted and reported as such. It is printed at the first
  candidate, then at most every 10 s, and in the runtime report at exit.

Any other value of the variable stops. On other platforms, setting it stops.

## Use

Run the title-idle repro (and gameplay) with `BB_COND_WAKE_CHECK=1`. Look for candidates on
`main` with caller 0x2083ecd (the engine's condition helper, under the event wait 0x207e8f0).
Those are the waits the kick's synchronous path relies on.

Candidates are observations of host behaviour: returns with no notification. They do not by
themselves establish that the guest may rely on no spurious returns, nor approve a new wait
policy. Only if the guest/PS4 contract is established (docs/macos/cond-wake-audit.md: FreeBSD's
user and kernel paths differ, and Sony's is unknown) would a change to the guest cond wait follow.
Its candidate design, explicit waiter grants rather than a generation loop, is in that audit.

## Test

`tests/test_cond_wake_check_macos.sh` drives the exported scePthread functions three ways: off,
on, and traced. It checks that none of these is a candidate:
- a real signal;
- a broadcast to two waiters;
- a signal to one of two waiters;
- a timed expiry.

It also checks:
- A native wake that bypasses the guest signal (what a host spurious wake looks like) is exactly
  one candidate. It names the waiter's pthread, guest caller (including through the trace's
  record) and cond.
- A cancelled wait leaves later waits counted as before.
- A paused signaler is ambiguous, not a candidate: its notification is counted before the wait
  begins, and its native wake happens after.
- Broadcast, then destroy and recreate the cond before the woken waiter reacquires the mutex:
  the waiter checks its own record.
- errno and the guest errno are kept although the report clobbers them.
- A reporter cancelled during its report: the guest mutex is released by its cleanup, and
  another candidate is reported and the summary completes.
- More than 64 sites: the rest are counted.
- If Darwin refuses the destroy, that control prints UNEXERCISED instead of PASS.
