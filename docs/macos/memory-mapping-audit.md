# Guest mapping overlap audit

This audit targets the command-buffer allocator's overlapping-pool hypothesis.
It does not infer that a torn command stream was caused by a mapping bug.

## Live address space

Direct/flexible maps, reservations and batch mapping operations enter the same
VMA writer lock. Nonfixed placement is first-fit from the hint; the hint cannot
replace a live view. Direct physical allocation is independently first-fit under
that lock. Flexible physical pages use a bitmap under the same lock. Intentional
direct aliases are supported and must not be confused with two overlapping VAs.
The low host-object allocator is separately serialized and occupies the 32-GiB
arena, not the command ring at 448 GiB and above.

On Apple, each placement now validates sorted, disjoint VMA extents, independently
scans for forbidden overlap before Mach's overwrite mapping, and checks both
neighbors at VMA insertion. Exhausted VMA metadata allocation aborts: returning
from that point could leave a host mapping changed without corresponding metadata.
The insertion checks also cover splits from protection changes, unmaps and release.
These are mutation-time checks, with no added per-draw region-query work. Placement
adds O(number of VMAs) work; it does not scan every allocation on every CPU access.

Fatal reports include requested start/size, flags, existing VMA range/kind/protection/
backing offset and a bounded alias snapshot (eight other guest views, total count,
truncation and generation). The query excludes host-only backing/import mirrors.
It reports current table state, not a historical proof about a previously queued
operation. Under the writer lock, an odd generation means a mutation is in progress.

## Fixed and reservation semantics

`FIXED=0x10`, `NO_OVERWRITE=0x80`. Current supported behavior:

| Request | Result |
|---|---|
| Nonfixed, including NO_OVERWRITE alone | First free range at/above hint; live mappings retained |
| FIXED + NO_OVERWRITE over committed mapping | Guest NO_MEMORY; old mapping retained |
| Bare FIXED direct/flexible map over committed mapping | Replace exactly that interval; neighbors retained |
| Bare FIXED direct/flexible map over reservation | Commit that interval |
| FIXED + NO_OVERWRITE over reservation | Apple hard stop: unqualified semantics |
| ReserveVirtualRange, bare FIXED over an existing VMA | Apple hard stop: unqualified semantics |
| ReserveVirtualRange, FIXED + NO_OVERWRITE over reservation | Apple hard stop: unqualified semantics |

The last cases deliberately do not guess PS4 behavior. The preexisting runtime
allowed NO_OVERWRITE to commit a reservation, but refused overlapping fixed
reservations. Upstream shadPS4's MapMemory treats Reserved as not Free for
NO_OVERWRITE, and its fixed CreateArea path unmaps the old area, including for a
new reservation. Neither emulator is console ground truth. A live occurrence must
stop with the ranges/flags so that the requested behavior can be investigated.
Linux and Windows retain their existing rules.

Primary implementation references inspected during this audit:
[shadPS4 memory manager](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/memory.cpp),
[VMA kinds/IsFree](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/memory.h), and
[kernel mapping entry points](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/libraries/kernel/memory.cpp).
These are public upstream implementation evidence, not official PS4 specifications.

## GPU label writes

`runtime_memory_write_backing` acquires the same VMA read lock that excludes map,
unmap, protect and release writers. Each segment does a fresh `vma_index` lookup;
it never uses the per-thread CachedRegion used by read-only renderer queries and
retains no VA-to-backing translation after returning. Fixed-remap and release tests
exercise this path through actual Mach aliases.

Apple additionally preflights coverage before copying. An unbacked span returns 0
without a partial write, preserving the documented TryWriteBacking contract; this
is needed for legitimate low/untracked mappings handled by its caller. Overflow
or a tracked translation outside its backing partition aborts with the same report.
Preflight adds one range walk under the existing read lock per backing write (one
extra binary lookup for the usual single-VMA label); no GPU wait is added.

This establishes current-translation correctness, not the intended lifetime of a
queued label: if a producer legitimately reuses a VA before an old label executes,
the old operation has no saved generation with which to identify that misuse.
GPU lifetime hooks drain work before remapping. Renderer separately audited
single-shot label execution and flip/idle waiting; no late-label route was found.

## Scope limits

The imported name `mmap` is not a registered host runtime implementation in this
checkout. Reaching it fails through unsupported-import handling; it does not fall
through to Darwin mmap. Native guest heap callbacks manage their own suballocations.
Two game pools/chunks overlapping *within one live 128-MiB VMA* are invisible to the
runtime VMA checks. The CMD-OVERLAP and builder trace diagnostics are required for
that case. No mapping-overlap root cause has yet been observed.

## Validation

`sh tests/test_memory_lifetime_macos.sh`: strict SDK-only x86_64 build, ad-hoc
signature verification and Rosetta run on macOS 27.2. Tests cover nonfixed hints,
no-overwrite committed rejection, exact middle-page fixed replacement with intact
neighbors, supported reservation commitment, and both unqualified reservation
stops. Injected overlapping insertion, corrupt extent and invalid backing
translation each abort and name the requested/existing range and alias state.
A deliberate two-view alias report confirms the diagnostic distinguishes physical
aliasing from forbidden virtual overlap. Current-backing writes after remap leave
the old physical alias untouched. A partly unbacked write makes no partial change.
Existing bounded alias, partial release/zeroing and 20 GPU-lifetime transactions
also pass. Tests do not launch a game or require a GPU build.
