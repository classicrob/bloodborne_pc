# Movie resource lifetime: heap lock audit

The fault dump at movie PC `0x26ae211` identifies a stale resource-list entry
pointing into a free GFX heap chunk (see `movie-resource-index-fault.md`). It does
not establish a duplicate live allocation. This audit tests whether our host
synchronization could fail to exclude two users of that heap.

## Actual GFX heap lock

Static decoding of the EU v1.09 eboot identifies heap state `0x805699e78` in the
GFX heap object (image offset `0x5699e48`). Allocation/free wrappers at
`0x1fc5ed0`, `0x1fc5f30`, and `0x1fc5fc0` lock their parent object's `+0x468`
field before calling the heap and unlock afterward. Its vtable `0x53aed80`
selects `0x20368c0` / `0x2036a40`, a guest CAS lock plus a kernel semaphore,
not an HLE pthread adaptive mutex.

The lock word holds a low 40-bit owner and high 24-bit waiter count. After its
spin budget, a contender atomically increments that count and waits for one
token from a semaphore created with attribute 2, initial count 0, maximum 1.
Unlock atomically decrements a pending waiter and installs an all-ones owner
sentinel before signaling one token. With no waiters, it clears the owner.
A successful waiter inherits ownership without writing its thread ID.

`runtime_sema.c` serializes count changes, waiter enrollment/removal, cancel,
delete and timeout under one mutex. A successful signal consumes the token and
removes its waiter before waking it. Later cancellation cannot wake that same
waiter again; deletion retains storage until all active waits return. No
extra-token path was found in this need-one/max-one pattern. Overflow currently
returns EINVAL without changing the count; this is not a console-qualified
claim about the exact overflow errno. Attribute 2 is served FIFO here; guest
priority ordering is not implemented or qualified by this equal-priority test.

## pthread type 4 is not glibc NORMAL

[FreeBSD 9 libthr](https://raw.githubusercontent.com/freebsd/freebsd-src/releng/9.0/lib/libthr/thread/thr_mutex.c)
`mutex_self_lock` explicitly groups ADAPTIVE_NP with ERRORCHECK: untimed self-lock
returns EDEADLK, timed self-lock expires. `mutex_unlock_common` checks the owner
and returns EPERM for a non-owner. Thus switching type 4 to Darwin NORMAL on the
basis of glibc behavior would remove a relevant owner check.
[shadPS4's mutex implementation](https://raw.githubusercontent.com/shadps4-emu/shadPS4/main/src/core/libraries/kernel/threads/mutex.cpp)
also retains these self-lock and unlock checks. These are implementation
references, not measurements of a physical PS4. FreeBSD 9's separate self-trylock
switch does not explicitly list ADAPTIVE_NP, so it alone does not settle that
case. Spin tuning and scheduling are separate from owner exclusion.

Current mapping: guest types 1 and 4 use ERRORCHECK, 2 uses RECURSIVE, 3 uses
NORMAL. The fixture verifies type 4 EDEADLK/EPERM and timed self-lock expiry.
It does not qualify NORMAL non-owner unlock (undefined by the host contract),
priority protocols, or every libkernel error against a console.

The game's wrapper is also informative. At `0x2084110`, Lock panics on EINVAL,
EDEADLK and EAGAIN, while timeout returns -3. TryLock at `0x20841d0` maps EBUSY
to -2 and panics on those other errors. Unlock at `0x2084290` deliberately
accepts both zero and EPERM; EINVAL panics. The observed EOF panic was Lock's
line 130 EINVAL, not non-owner unlock EPERM. No mutex semantic change follows
from this audit.

## Regression and result

Run through the machine limiter:

```sh
tools/macos/heavy.sh heap-lock -- sh tests/test_guest_heap_lock_macos.sh
```

The signed SDK-only x86_64 fixture links actual memory, thread, semaphore and
mutex implementations. On macOS 27.2 / Rosetta it passed:

- Eight workers, 32,000 protected counter/list transactions for the decoded guest
  adaptive algorithm, with 31,999 slow-path semaphore handoffs in the final run.
- The same transaction test for each pthread type 1 through 4, mixing lock,
  try-lock and timed lock; distinct low guest thread identities.
- Recursive/owner/self-lock/timeout controls, including rejected non-owner
  unlocks for types 1, 2 and 4.
- Maximum-one semaphore overflow preserving its token, timeout cleanup, eight
  enrolled waits released exactly one at a time, cancellation and reuse.

Sol-2 reviewed the fixture and semaphore paths. Review found an independent
C data race in plain global mutex diagnostic counters, including an increment
after native unlock. Those counters and report reads now use relaxed atomics;
this fixes bookkeeping UB without changing synchronization semantics.

Passing this finite test does not prove the guest's resource lifetime correct.
The next evidence needed is which queued render object retains the freed
resource, and which destructor/context move ends its lifetime.
