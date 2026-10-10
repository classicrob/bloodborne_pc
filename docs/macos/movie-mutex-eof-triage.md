# Opening-movie EOF mutex panic

The default-config `8cf9ed7` acceptance run
`20261010-191304-1080p-ACCEPT2-8cf9ed7` panicked at the game's
`PthreadMutex.cpp:130` with EINVAL after demux EOF and audio-decoder exit.
The video-decoder exit was not logged. This is separate from the earlier host
`std::jthread` join race. The current evidence does not prove which thread or
operation invalidated the mutex.

## Static provenance

Read-only disassembly of the shared v1.09 eboot establishes:

- The panic return at image offset `0x208418d` follows the EINVAL branch of
  the game's mutex lock wrapper `0x2084110`.
- Caller `0x263a1bf` supplies timeout `-1`; the wrapper takes its blocking-lock
  branch. PLT `0x2bbfef8` resolves through its relocation to
  `9UK1vLZQft4#p#J`, `scePthreadMutexLock`.
- `0x263a1b0` locks the passed object's mutex, reads its dword at `+8`, and
  unlocks. At the failing caller `0x29004e0`, that object is worker base `+0x28`.
- The worker loop calls PLT `0x2bc01c8`, relocation `Wnp1OVcrZgk#m#n`,
  `sceAvPlayerGetAudioData`, before its next state read. It is the guest's audio
  polling worker, not the host AvPlayer controller callback.
- The guest thread wrapper `0x263b1e0` invokes that worker and is reached through
  runtime `host_start`, matching the reported stack.
- Nearby worker cleanup `0x29002d0` changes state from 1 to 2 and polls until 3;
  the worker writes 3 after leaving the loop. Object destructors at
  `0x2900160` / `0x29001c0` destroy the mutex at `+0x28`. The failing stack alone
  does not establish whether that cleanup/destructor ran or whether storage was
  otherwise overwritten.

No proprietary instruction bytes are included here. The deliberate final fault
at `0x20b56d2` is the game's panic, not the original invalid memory access.

## Diagnostics

Apple runtime mutex, condition-variable, attribute and rwlock exports now report
all EINVAL returns. SCE and POSIX results are unchanged. The line includes:

- outer API operation and object kind;
- handle-slot address and safely read handle value/state;
- native object's first word when readable, useful for Darwin's signature;
- associated mutex for condition waits;
- outer ABI caller return address, thread pointer/name and guest result;
- native pthread EINVAL origin, or `none(validation)` with native errno zero.

These are **post-error snapshots**, not a lifetime registry or proof that the
same handle value was used earlier in the native call. Init output storage is not
read. Diagnostics use Mach reads so reporting an unreadable object does not add a
speculative C dereference. The normal path adds result checks, with no logging,
Mach reads, history lock, or extra allocation. Other platforms retain their
existing export targets and native calls.

`sh tests/test_sync_diagnostics_macos.sh` builds the actual runtime implementations
and Mach low allocator using only the SDK, signs the executable, and runs under
Rosetta. Sentinel rejection, native-destroyed mutex/condition errors, rwlock
lifetime errors and POSIX error values pass. An executable low-memory caller
verifies its exact return address appears in the diagnostic. Valid lock/unlock
produces no log, and a validation error following a native error does not inherit
its native origin.

This unit is instrumentation, not a fix for the live movie lifetime failure.
A recurrence with these diagnostics must distinguish guest destroyed-handle
rejection from Darwin object rejection before changing synchronization semantics.
