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
  destroying several contexts. That helper queues resource nodes via 0x26aa5a0 and clears
  their context list heads, and submits through 0x26b0b00. Queueing retirement is different
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
