# Guest lifetime paths relevant to submit-throttle relaxation

Static audit of the local Bloodborne 01.09 image. Addresses below are image offsets; add
`0x800000000` for this port's guest addresses. This extends the guest lifetime section in
[gpu-idle.md](gpu-idle.md). It does not establish that removing the throttle is safe.

## Immediate resource-range frees: 0x26aa2e0 and 0x26aa440

Both routines have the same decoded operations. RDI is the resource-range allocator A and
RSI is a 48-byte metadata node N. The payload being made reusable is initially:

```
start = N[+0x08]
end   = N[+0x18][+0x08]
```

The routines decrement A's allocation count at +0xc8 and add end-start to its free-byte
count at +0xd0. They coalesce adjacent nodes whose state qword is zero, return redundant
metadata nodes through 0x263b9b0, repair the address-ordered/free-list links, and set the
surviving node's state to zero. They do not unmap the payload. Later allocations can reuse
that range, so freeing the metadata is not the only lifetime effect.

The entry calls the allocator's lock at A+0xd8 (virtual +0x18, timeout -1); the exit releases
it through virtual +0x28. There is no label-value-4 test, flip-status query, GPU event wait,
or submission in either routine. This is allocator exclusion, not a GPU completion gate.
The normal label-governed reclaimer 0x26aa860 remains a different path.

These ranges can hold execution-time GPU inputs. The previously decoded allocation/lookup
chain 0x26d0d30 / 0x26d0ff0 returns node+8 and reaches INDEX_BASE through 0x26bba89 and
0x10752c0. Submit-time command snapshots do not preserve those index bytes.

### Direct callers and virtual exposure

| Caller | What it frees | Completion check at this layer |
| --- | --- | --- |
| Context destructor 0x26aec40, calls at 0x26aed8b/0x26aedcb | Nodes from context+0xb3d8 bucket heads selected by +0xb460, then the list at +0xb458; allocator is context+0xb3d0; next node saved from +0x28 before freeing | None around these loops |
| Context cleanup 0x26af3f0, calls at 0x26af43b/0x26af47b | The same bucket/list structure, then clears the heads | None before these loops |
| 0x216af60 -> 0x26aa2e0 | Forwards the supplied node using wrapper+8 as allocator | None |
| 0x216aef0 -> 0x26aa440 | Same forwarding shape | None |

The last two are vtable slot +0x28 at tables 0x535f350 and 0x535f300 respectively (verified
from loader relocation records). Therefore the direct-call list alone is not an exhaustive
list of guest users of immediate free.

Known caller distinctions matter:

- Initializer 0x26aee20 branches to cleanup at 0x26aefa4 when 0x26bbe10 or 0x26adb50 returns
  an error. That is an initialization rollback path; its existence is not evidence of a live
  GPU lifetime violation.
- Wrapper cleanup 0x21682c0 calls 0x26af3f0 at 0x216830a after disposing of its allocator
  wrapper and resource-lookup records. The wrapper contains no explicit completion wait.
- Context factories also destroy temporary contexts after copying them into persistent ones
  (for example 0x2165534 -> 0x21655b2 and 0x21677ea -> 0x216783a). A destructor hit alone
  does not prove that it frees a live payload: list contents and ownership transfer matter.
- The large renderer cleanup at 0x2196e10 calls the flush/retirement helper 0x26af700 before
  destroying several contexts. When its callback/list guards pass, that helper queues
  resource nodes via 0x26aa5a0, clears their context list heads, and can submit through
  0x26b0b00. Queueing retirement is different
  from waiting for its GPU completion; emptied lists also change what subsequent immediate
  cleanup can actually free. This is not a proof that every entry to cleanup is protected.

Thus neither immediate-free routine supplies its own GPU lifetime predicate. Whether a
particular call is safe depends on its node being unused, already retired, or removed from
these lists by the normal retirement path. The current static evidence does not establish
those preconditions for all indirect callers, nor that SubmitDone is the sole barrier they
rely on. Retaining the throttle is conservative; claiming it proves every teardown safe
would also go beyond this audit.

## Command-chunk claim: 0x26d3b80

Allocator layout:

| Offset | Field |
| --- | --- |
| +0x08 | q: chunk size in qwords |
| +0x10 | n: number of chunks |
| +0x18 | pool base |
| +0x20 | scan cursor |

Chunk i begins at base + 8*i*q. Its status is the **64-bit** word at
base + 8*(i*q + q - 1). The allocator scans at most n chunks beginning at cursor+1 modulo n.
It reads status and, only when it sees zero, calls 0x207ebc0 with expected=0 and desired=1.
That helper is **lock cmpxchg qword**, not xadd. A losing claimant does not increment or
otherwise change the status. On success it updates the cursor, returns the chunk through
RSI, and returns zero. If none is claimable it returns 1 when it saw a released status 2,
otherwise 2; an empty pool also returns 2. It neither sleeps nor queries flip/event state.

Ordinary states are 0 free, 1 claimed, 2 released awaiting a GPU write. There are two packet
release helpers, both CPU-storing qword 2 before emitting the release:

| Helper | Packet path | Completion write |
| --- | --- | --- |
| 0x26d3c80 | 0x26bb6d0 -> 0x10778f0; EVENT_WRITE_EOP header 0xc0044700, event field 4 | data_sel 2, qword zero at chunk end-8 |
| 0x26d3cb0 | 0x108b0f0; RELEASE_MEM header 0xc0054902, event field 0x2f | data_sel 2, qword zero at chunk end-8 |

The graphics release is reached while flushing context record/chunk lists (including
0x26af83e, 0x26afe48, 0x26afed9, 0x26aff02 and 0x26b067f). The RELEASE_MEM variant is
called from 0x26b180c while walking the secondary context's chunk list.

A separate helper 0x26d3d00 polls the status until zero, yielding through 0x207f970 while
nonzero. A direct CALL/JMP scan found no callers; its mere existence cannot be used as the
precondition for all releases.

For the ordinary packet-release path, reuse is gated by the status label itself. If that
write is delivered once, at the required GPU completion point, and nobody independently
zeros the status, a chunk cannot be reclaimed before the completion write. The CAS makes
CPU ownership exclusive after that point. No additional GpuIdle wait appears inside claim.

### The separate immediate-reset exception

0x26d3d30 directly stores a qword zero to the same status location, without a wait or packet.
Direct callers are:

- 0x26af4c6, 0x26af56a, 0x26af5fc in context cleanup 0x26af3f0: the current chunk, recorded
  CommandBuffer chunks (deduplicated), and the separately tracked chunk list.
- 0x26b131a and 0x26b1399 in secondary-context cleanup 0x26b12f0: the current chunk and
  saved chunk list.

0x26d3d40 only rounds an interior pointer down to its chunk base; it is not a wait. The
cleanup uses it before some direct resets. Pool destruction 0x26d3b50 similarly frees the
backing pool through its allocator without polling its status words locally.

This means the **ordinary status protocol is completion-governed, but the whole allocator
lifecycle is not**. If cleanup resets a status while an old zero-writing packet remains
pending, a new claimant can take the chunk and that old packet can then zero its live
status. Command snapshots do not prevent this status-word hazard. This is a possible
ordering failure to exclude, not evidence that a recorded run has suffered it.

Also, snapshots protect command packet bytes only. A command chunk can contain end-allocated
labels or other addressed data; an allocator name does not prove every byte is consumed only
by the parser. GPU-readable data placed there needs the corresponding completion lifetime.

## Consequence and remaining observation

The unknown chunk row can be narrowed: **normal reuse through EOP/RELEASE_MEM is safe under
correct completion writes; direct reset and pool teardown still need caller qualification.**
The two resource-range frees have no local GPU predicate and remain conditional on caller
ownership/retirement.

To close those remaining cases, trace actual nonempty immediate-free/reset calls with:
node/payload range or chunk/status address; caller and thread; the node/list ownership; and
which preceding retirement packet/submission, if any, is still in flight. Useful framed
entries are 0x26aa2e0, 0x26aa440, 0x26af3f0 and 0x26b12f0. The leaf reset 0x26d3d30 has no
push-rbp prologue and cannot be installed in the existing entry probe unchanged.

A wait in a higher caller must be tied to the work using that exact range. A clean movie or
clinic run, an empty destructor list, or a submit return alone does not establish that rule
for scene transitions and shutdown. No throttle policy change follows from this note.

## Follow-up: caller lifetime audit

Audited against extracted image SHA256
`24935ac6da5e1879215367a42cb7d90a91fcc6bf63ea5cc140904db413d5f681`.
The tables below cover the direct CALL/JMP callers found for the cleanup/destructor
routines, plus the higher concrete child-context teardown chain. They are not a whole-program
proof of indirect call targets. In particular, a function with no direct callers is not
thereby proved unreachable.

**Result: no completion-wait proof for all non-initialization callers.** Initialization
rollback accounts for most pool-destruction sites. There is also a concrete cloth-manager
teardown path that calls the flush helper and then cleans up the child without a CPU
GPU-completion wait between those operations. Its mode-1 child normally fails the helper's
submission guard; successful child finalization can instead transfer chunk ownership away.
Whether teardown destroys a chunk with outstanding GPU references
still depends on runtime ownership and pending work. This is a conditional existing hazard,
not proof that R1 introduces a new hazard.

### Immediate frees and primary direct resets

`0x21682c0(W)` cleans up the primary CommandBuffer at `W+0x10`. Before calling
`0x26af3f0` at `0x216830a`, it destroys W's allocator adapter and frees lookup-record
storage with `0x26d0ca0`; neither operation tests GPU completion. The synchronization at
the end of `0x21682c0` protects the context-ID registry and occurs **after** the frees/resets.
Within `0x26af3f0`, the helper between resource frees and the current-chunk reset is
`0x26bc2d0 -> 0x26dbc10`; the latter is just `ret` in this image.

| Caller / call site | Ownership or preceding operation | GPU completion established? |
| --- | --- | --- |
| `0x26aee20 -> 0x26af3f0` at `0x26aefa4` | Fresh-context initialization fails in `0x26bbe10` or `0x26adb50`; record/chunk lists were initialized empty. The mode-0 path may have claimed a fresh chunk and built initial commands, but has not submitted them here. | No wait; rollback of newly constructed state, rather than retirement of submitted work. Requires the normal fresh-object initialization contract. |
| `0x2168080 -> 0x21682c0` at `0x2168154/0x2168281` | Context-ID/adapter/lookup initialization failure. | No wait; initialization rollback. |
| Factories at `0x216560c/0x2167813` | The first is actually inside destructor `0x21655f0`; the second is child-factory allocation failure after initializing a temporary context. | `0x2167813`: rollback. `0x216560c`: normal teardown, no local wait. |
| `0x2167960 -> 0x21682c0` at `0x2167969` | Standalone wrapper destructor; then lookup/state destructors and tail-call `0x26aec40`. | No local wait or retirement predicate. No direct caller found for `0x2167960`; dynamic reachability remains unproved. |
| `0x26d5810 -> 0x21682c0` at `0x26d5819` | Same destructor shape for the larger child context. Called by cloth-manager cleanup and renderer cleanup, detailed below. | No local wait. |
| `0x216b900 -> 0x21682c0` at `0x216b911` | Full owner teardown starts by cleaning the supplied primary context, before secondary-context cleanup or pool destruction. | No preceding wait in this function. |
| Renderer cleanup at `0x219819c` | Follows `0x216b900`, which has already cleaned that primary context. | No new wait; the second cleanup normally sees cleared lists/current pointer. The first cleanup remains the relevant lifetime boundary. |

The immediate-free calls in `0x26aec40` are reached from `0x21655b2`, `0x2165630`,
`0x216783a`, `0x21981c0`, and tail calls `0x2167993/0x26d5843`. The two factory temporary
calls (`0x21655b2/0x216783a`) follow initialization/copy-out without rendering through the
temporary object. The remaining calls follow the wrapper cleanup described above; they add
no completion wait. Normally that preceding cleanup has already emptied the resource lists.
Consequently, instrumenting only `0x26aec40` misses the earlier lifetime boundary.

The allocator adapters `0x216aef0/0x216af60` remain a separate coverage boundary. They are
installed in `W+8` by `0x2168080`, at vtables `0x535f300/0x535f350`, respectively. Slot
`+0x20` is the normal label-retirement operation; slot `+0x28` is immediate free; slot `+0x30`
can use an external completion label. No adapter-level wait exists. This audit does not
resolve every indirect `+0x28` call to these particular receiver types, so it cannot certify
all users of `0x26aa440` merely from its absence of direct CALL instructions.

### Secondary direct resets and pool destruction

| Caller / sites | Classification | GPU completion established? |
| --- | --- | --- |
| `0x26b1130 -> 0x26b12f0`, `0x26b1263/0x26b126d/0x26b127d` | Secondary initialization fails on claim, allocation, or queue setup. No compute work has been rung from this initialization path. | No wait required for submitted work from this new context; rollback only. |
| `0x216b0f0 -> 0x26b12f0`, `0x216b895` | Later owner-initialization failure after secondary construction. | Rollback, no completion predicate. |
| `0x216b900 -> 0x26b12f0`, `0x216b935` | Full teardown; directly zeros current/saved chunk statuses at `0x26b131a/0x26b1399`. | No local status poll or GPU wait before the resets. |
| `0x216b0f0 -> 0x26d3b50`, `0x216b41f/43e/468/4ac/4cc/640/660/8a9/8cc` | All nine sites are owner-initialization unwind paths. Pools were allocated in this initializer; the failure paths free them before handing the owner to its caller. | No wait; no submitted work from these new pools identified on these paths. This does not qualify reinitializing a live owner in place. |
| `0x216b900 -> 0x26d3b50`, `0x216baf0/0x216bb1b` | Normal teardown of owner+8 and owner+0 pools, after direct context resets and resource-allocator destruction. | No pool-status scan or range-specific completion predicate. |

`0x216b900` has four direct callers: owner-factory allocation failure at `0x2165588`,
owner-initializer failure at `0x216b722`, owner destructor at `0x2165604`, and renderer
cleanup at `0x2198194`. Only the first two have the fresh/unpublished-owner rollback
argument. The latter two need an enclosing lifetime precondition. `0x26b9c10`, called
conditionally during teardown, is just `ret`; it is not a hidden idle wait. The other shared
cleanup helpers free reference-counted library state; their presence is not a wait for this
owner's command pools.

### Concrete child-context teardown chain: cloth manager

The singleton at `0x54b46f0` is named `FD4HkClothManager` by the adjacent guest diagnostic
strings. Its teardown is reached through destructor `0x1e3a780` (vtable `0x534f0e0`; the
other destructor entry is `0x1e3a760`). The decoded chain is:

```
0x1e3a780 --call 0x1e3a7f5--> 0xfd3050
0xfd3050 --call 0xfd3096--> 0x10562d0
  for each child in owner[+0x10], when owner[+0x80] == 0:
    0x1056396 -> 0x2168530(child)       // calls guarded flush helper
    0x10563cd -> 0x26d5810(child)       // destroy
      0x26d5819 -> 0x21682c0(child)
        0x216830a -> 0x26af3f0(child+0x10)
          resource frees, then direct chunk-status resets
```

There is no explicit GPU label/flip/idle wait in this decoded chain between the flush and
cleanup. `0x10562d0`'s initial `0x207f870` returns true, and the subsequent `0x207f860`
returns zero immediately. They do not join a worker or wait for GPU completion in this
image. `0xfd3050`'s preceding `0x1056b90` also just frees a CPU container. A higher caller
may still impose quiescence: the virtual destruction of the object at `0x1e3a780`'s `+8`
and the lifecycle that invokes this destructor have not been resolved into a GPU wait.
Thus this is a named no-local-wait path, not a demonstrated in-flight violation.

`0x10562d0` also has direct callers `0x105625a` (its owner destructor) and
`0x105671f/0x1056768` (construction rollback in `0x1056580`). The last two must not be
counted as evidence of freeing submitted work.

### Child finalization transfers ownership rather than waiting

The cloth initializer `0x1056580` creates its children through `0x21676c0` at `0x10566c7`.
That factory uses mode 1, whose initializer installs callback `0x26af230` at
`0x26aeefe`. The submitting mode-0 callback is `0x26af030`, installed at `0x26aef52`.
The flush guard below distinguishes them: merely calling `0x2168530` on this child does
not establish that a submission occurred.

The cloth child-finalization loop has this separate path:

```
0x10567e0 --call 0x1056812--> 0x26d5850(child, output, 0)
  0x26d587b -> 0x216aca0
    0x216acf7 -> 0x26afd30(child+0x10, output)
```

On the successful path, `0x26afd30` emits chunk-release packets into the command stream,
appends the current CommandBuffer record if necessary, and clears the source's begin/end,
cmdptr and saved begin at `0x26b0027/0x26b002b/0x26b0033`. It transfers the record allocator,
head, current block, capacity and used count to the output at `0x26b00a7..0x26b00d8`, then
zeros the source's used count and both record-list pointers at `0x26b00dc..0x26b00f2`.
The source now has neither that current chunk nor those recorded chunks to reset during
cleanup. This is a concrete ownership-transfer explanation for an empty cleanup without
a GPU wait; it is not GPU retirement of the transferred chunks.

The companion cloth loop `0x1056830` passes the outputs to the renderer's context through
`0x105687d -> 0x21679a0 -> 0x216a7d0`. Establishing retirement of all their addressed data
still requires following the receiving context's ownership and submissions. Moreover,
error-9 exits in `0x26afd30` can precede its clear/transfer, and the loop at `0x1056812`
does not branch on the returned error. Successful finalization therefore qualifies the
transferred current/record chunks only; unfinished or failed finalization, resource-range
nodes, and any separately retained chunks still need a lifetime argument. This audit does
not prove that every cloth teardown follows successful finalization.

### What the flush proves, and what it does not

`0x2168530` and the inlined renderer cleanup sequences call `0x26af700`. Its substantive
retirement branch requires **both** context+`0xb4d8` nonnull and callback `0x26af030`
(`0x26af72d..0x26af746`, compared through relocated GOT slot `0x53e43c8`). Otherwise it
returns after saving cmdptr at context+`0xb4b8`. In particular, a factory child retaining
callback `0x26af230` does not enter this branch. Inside the branch it queues the
resource nodes for label-governed retirement, clears those resource heads, emits chunk
release packets and clears the saved-chunk list. It can submit through
`0x26af9c2 -> 0x26b0b00 -> 0x1089a10` (SubmitCommandBuffers). No CPU completion wait
follows that submit. The `0x1077b30` calls in the submit helper build GPU wait packets;
they do not make the CPU wait for the just-submitted work.

The flush preserves a current CommandBuffer pointer, advancing begin to cmdptr after
submission. `0x26af3f0` rounds its nonnull begin back to a chunk and directly zeros that
chunk's status. An emptied resource/saved-chunk list therefore does not, on its own,
prove the remaining current chunk has no execution-time references. After the flush,
`0x26bba10/0x26d3300/0x26d2d20` only reset cached state; they are not completion checks.
The renderer cleanup sites `0x21974ef/0x219768f/0x2197822` have this flush-then-destroy
shape. Their outer lifecycle and any indirect callbacks still require qualification.

For a context that actually submits through this guard, R0's pre-submit GpuIdle throttle
does not turn the flush's return into completion of that flush. If the direct reset frees
a live current chunk here, it is possible under R0 too. The cloth child's call to the
helper alone does not demonstrate this submitting-context case.
R1 may change the overlap window, but calling this an R1-only hole would be incorrect.
Conversely, finding no change in a label checker does not prove the chain safe: a reset
before parse, ABA, or destruction without a subsequent write can remain invisible.

For a decisive trace, capture `0x10562d0`, `0x26d5810`, `0x216b900`, `0x26af3f0` and
`0x26b12f0` with caller/thread, the actual current/list chunk addresses and statuses,
and the outstanding submission/label ownership of those ranges. The immediate-free
entries additionally need node+8/end and the adapter receiver/vtable. This identifies
nonempty live teardown and resolves the indirect-call boundary; a destructor entry alone
does neither. No source patch, game launch, or R1-default recommendation is part of this audit.
