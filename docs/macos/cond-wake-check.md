# Unnotified condition wakes (`BB_COND_WAKE_CHECK=1`, diagnostic)

Guest condition waits (`scePthreadCondWait`/`TimedWait`, `pthread_cond_wait`/`timedwait`) run
on host `pthread_cond_*` (src/runtime_mutex.c). On Darwin, a wait whose psynch/ulock call is
interrupted (by a signal, for example) returns success without a wake. The port interrupts guest
threads with signals: write faults, the CPU profiler and entry probes. Guest code that waits once
without rechecking its predicate then goes on early. The engine's event wait 0x207e8f0 does
exactly that (docs/macos/movie-handoff.md).

With `BB_COND_WAKE_CHECK=1` (macOS), each cond counts signal and broadcast attempts. The count
goes up before the native wake, so a waiter woken by it never finds it unchanged. A wait that
returns success with its cond's count unchanged since it began is an **unnotified wake
candidate**.

A changed count does not prove that this waiter was the one woken. One signal can overlap
several waiters, so the check undercounts. It never names a notified wake as a candidate.

Waits behave the same with the check on or off. The check:
- enrolls nothing per waiter, so a cancelled wait leaves nothing behind;
- holds no diagnostic lock across a wait;
- never takes the guest mutex in signal or broadcast.

Native signal and broadcast failures are counted separately, since a failed attempt notifies no
one.

## Output

The output goes to stdout.
- `COND-UNNOTIFIED candidate #n: thread "<name>" caller <guest return address> cond #<id> ...`
  is printed for the first 32 candidates.
  - The caller is the guest's return address from the sync import. It comes from the HLE
    trace's record when `BB_HLE_TRACE` or its summary replaced the return slot.
  - The cond id is unique per cond created, unlike its recyclable address.
- `COND-UNNOTIFIED summary` gives the count per thread, caller and cond. It is printed at the
  first candidate, then at most every 10 s, and in the runtime report at exit.

Any other value of the variable stops. On other platforms, setting it stops.

## Use

Run the title-idle repro (and gameplay) with `BB_COND_WAKE_CHECK=1`. Look for candidates on
`main` with caller 0x2083ecd (the engine's condition helper, under the event wait 0x207e8f0).
Those are the waits the kick's synchronous path relies on.

If they exist, the fix belongs in the guest cond wait. Its exact policy is in Astra-2's audit,
docs/macos/cond-wake-audit.md: explicit waiter grants, not a generation loop.

## Test

`tests/test_cond_wake_check_macos.sh` drives the exported scePthread functions three ways: off,
on, and traced. It checks that none of these is a candidate:
- a real signal;
- a broadcast to two waiters;
- a signal to one of two waiters;
- a timed expiry.

A native wake that bypasses the guest signal (what a host spurious wake looks like) is exactly
one candidate. It names the waiter's guest caller and cond, including through the trace's
record. A cancelled wait leaves later waits counted as before.
