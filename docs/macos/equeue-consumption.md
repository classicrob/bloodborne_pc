# Equeue poll consumption race

`EqueueInternal::WaitForEvents` previously called `GetTriggeredEvents` without
the queue mutex for a zero-microsecond timeout. That helper reads and mutates
the same event vector as registration, triggering and blocking waits, including
clearing pending state and deleting one-shot entries. This was a cross-platform
data race. A poll could consume an event while another waiter or trigger held
the mutex, potentially duplicating or losing a notification.

The fix takes the existing mutex for polling too. The shared consumption helper
is now private and named `GetTriggeredEventsLocked`; both call sites hold that
mutex. Blocking condition-variable waits release it as before. No GPU wait or
renderer callback is added inside the critical section. Cost: one mutex
lock/unlock per zero-timeout poll; scan cost is unchanged. This is not a fix to
the independent global equeue registry lifetime or small-timer selection issues.

## Semantics and regression

[FreeBSD kqueue](https://man.freebsd.org/cgi/man.cgi?query=kqueue&sektion=2&format=html)
coalesces multiple triggers of an ident/filter pair into one pending event;
EV_CLEAR resets its state on retrieval. Therefore arbitrary M triggers must not
be tested as M distinct deliveries. The fixture preserves this implementation's
filter-specific aggregation, as in the
[upstream event definitions](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/libraries/kernel/equeue.h):

- User level events remain readable; Clear events coalesce and reset;
  one-shot events are removed.
- GraphicsCore events carry the latest event data, normally the EOP event type,
  not an occurrence count. Static registration userdata is retained.
- VideoOut stores a four-bit occurrence count saturating at 15 and the latest
  flip argument; its timestamp bits are not compared to a deterministic value.
- Timer occurrence counts accumulate and reset on retrieval.

`tests/test_equeue_concurrency.cpp` first holds the queue mutex and asks another
thread to poll a pending clear event. The old shared library returned it before
the mutex was released: the negative control exited 1 with
`zero-timeout poll consumed an event while queue mutex was held`.

With the fixed production equeue translation unit, that control passes. Eight
consumers (four zero-timeout pollers and four finite blocking waiters) then race
two triggering threads across 32 event identities. A producer waits for the
previous payload on an identity to be acknowledged before retriggering it, so
legitimate coalescing cannot hide a missing notification or imitate duplication.
All 4096 payloads were consumed exactly once with identity, filter and flip
count checks. A 30-second alarm bounds deadlock or lost-event failures.

The signed x86_64 fixture passed under Rosetta on macOS 27.2. It compiled the
updated production `equeue.cpp` and used the stable shared library only for its
dependencies; no second GPU library was built. The CMake `equeue-concurrency-test`
target uses the rebuilt production library and is wired into Darwin
`build.sh --test`, including signing and the export gate. The wiring guard passes.

This demonstrates the race and its local repair. It does not establish that
duplicated completion events caused the movie context-copy failure; the live
entry trace and movie acceptance runs must decide that.
