# GpuIdle must mean idle

## Mechanism

`sceGnmSubmitDone` sets `submission_lock` unless the GPU is idle; the guest's next submission
waits for it (`WaitGpuIdle` in gnmdriver.cpp). The lock is cleared by the GpuIdle interrupt
(`ResetSubmissionLock`). The initial investigation inferred frame-based command-buffer reuse from
the canary observations. That was not a disassembly proof of an unconditional N-1 lifetime rule;
the guest label and flip checks found below qualify that interpretation.

`Liverpool::Process` raises GpuIdle at the end of every loop iteration. Before raising it, the
iteration drains the draw recording thread and waits for the fences that thread deferred, which can
take a long time while that thread lags behind (loading, pipeline compiles). If the guest submits and
calls `sceGnmSubmitDone` during that drain, the interrupt raised afterwards is stale: it clears the
new lock while the new submissions are still queued. The guest is released a frame early and
rewrites buffers the parser has not reached. Those packets are torn (`NextPacket: packet length
exceeds remaining submission size`), and eventually the parser fails with `Wrong PM4 type 0/1`.

How this explains the production runs (2026-10-10, Opus-Playtest):

- **Canary:** caught exactly this. A submitted top-level DCB had 4 words rewritten by the guest CPU
  about 9 submissions (one frame) before it was parsed.
- **Default config:** torn every time (6/6).
- **`PipelinedMemoryWrites` off:** clean 2/2. Its drains happen during parsing, so the final drain
  is short and the stale window nearly closes.
- **All pipelining off:** no draw pipe, so the drain takes no time.
- **Write-ordering fix (9e79075, reverted):** zero syncs and still torn. Stream writes into command
  memory were not the cause.

## Fix (Apple)

GpuIdle is decided and raised under `submit_mutex`, the lock submissions take, and only when no
submission is queued. Otherwise the loop processes the new submissions and raises GpuIdle once they
are retired, which is the PS4 meaning of the interrupt. Each held-back interrupt is logged (first 8 per
window). Every 600 frames, a summary logs the parse lag (graphics submissions queued ahead of the
parser at each submit: mean and max) and how often GpuIdle was held back. Linux and Windows have
the same code and the same race, with a shorter drain; the fix is Apple-only until it can be tested
there.

## Not the cause (kept for the record)

- **Submit-time snapshots of command memory:** the GpuIdle fix alone was not enough. A later
  run tore on a zeroed packet with the guest only about 2 submissions ahead. Snapshots were added
  on top (`docs/macos/command-snapshots.md`).
- **Stage A reading command memory or `COND_EXEC` predicates before stage B's `WRITE_DATA`/`DMA_DATA`
  land:** this ordering gap is real in principle, but no such write occurred at the tear points
  (9e79075 counted zero).

## Guest lifetime audit (2026-10-11)

Static analysis of the CUSA03173 eboot, SHA256
`d65f0b4f01d59166aed16f8604196d8b7dd805abbf0758b356e8f1354c9429f9`.
All PCs below are offsets from its load base (`0x800000000` in the observed runs).
Names describe the decoded behavior, not recovered symbols. No game was launched for this audit.

**The normal 48-byte-node reclaimer checks a completion label. It does not free a fixed frame's
objects merely because SubmitDone returned. These nodes can own index-buffer data, so a
recording-time label is insufficient unless the renderer has already made a stable data copy.**

### Submit, flip gate, and frame age

The import thunks are `0x1089a10` (SubmitCommandBuffers), `0x1089a20`
(SubmitAndFlipCommandBuffers), and `0x1089b50` (SubmitDone). The bool wrapper at
`0x1089b60` calls AreSubmitsAllowed. A direct-call/RIP-relative-reference scan found no caller
of that bool wrapper; this is not proof that all indirect paths or PRXs never use it.

Both direct SubmitDone call sites, `0x26b0e04` and `0x26b0ee7`, immediately follow
SubmitAndFlip. Their wrappers return without freeing nodes. Caller `0x26afb30` advances
the submitted command-buffer begin pointer to its current write pointer after that return.

The normal frame-end path `0x26d6e50` has this order:

1. Read frame serial `F = [object+0x2e0]` and display-slot count `B = [object+0x328]`.
   The selected slot is `F % B`. This audit has not pinned the live value of B.
2. Call GetFlipStatus (`0x26d6ee9`). Once `F >= B`, require
   `FlipStatus.flip_arg >= F + 1 - B`. Otherwise call WaitEqueue with a null timeout
   (`0x26d6f26`) and recheck status. The event alone does not satisfy the gate.
3. SubmitAndFlip with flip argument F, then SubmitDone, through `0x26afb30` at
   `0x26d6fbb`. On success increment F at `0x26d6fc8`.
4. Call the pending-node reclaimer `0x26aa860` at `0x26d6fe8`, then context reset helpers.

The reclaimer scans pending lists without a frame-number comparison. It can reclaim a completed
node from the just-submitted frame or any older frame; unfinished nodes remain pending. There is
also an allocation-failure retry that calls the same reclaimer at `0x26d0d92`, independent of
frame end. Consequently, waiting for frame idle only at a frame boundary cannot by itself protect
against early completion labels and reuse inside a frame.

### The 48-byte node allocator and completion test

| PC | Decoded operation |
| --- | --- |
| `0x26a9b50` | Initialize the resource-range allocator; at `0x26a9ba1` pass element size `0x30` to its node pool initializer `0x263baa0`. |
| `0x263b8b0` | Pop a node from the pool; `0x263b8e7` reads its offset-zero free-list link, the historical crash site. |
| `0x26a9db0`, `0x26aa070` | Allocate/split resource ranges using these nodes. A live node has state 2, payload start at +8, physical neighbors at +0x10/+0x18, and list links at +0x20/+0x28. |
| `0x26aa5a0`, `0x26aa620` | Queue retirement on owner+0x98/+0xa0, set node[0] to 2, and append an EOP targeting node[0]. |
| `0x26aa6a0`, `0x26aa780` | Same retirement, or store an external label pointer OR 1 in node[0] and queue on owner+0xa8/+0xb0. The external-label branch emits no local EOP here. |
| `0x10778f0` | EOP builder. The above local-label callers pass event 0x28, data selector 1 (32-bit), and value 4; this is not a 64-bit frame-counter write. |
| `0x26aa860` | Drain four pending lists under the allocator lock, inspect completion, coalesce completed resource ranges, and requeue unfinished nodes. |
| `0x263b9b0` | Return metadata node to pool, placing the next free-list pointer at node[0]. |

At `0x26aa970..0x26aa984`, the reclaimer loads node[0]. If bit zero is set, it loads a
32-bit value through `node[0] & ~1`; otherwise it uses the inline value. It proceeds only if
the low 32 bits equal **4**. Coalescing returns redundant 48-byte metadata nodes to the pool
at `0x26aa9c4`/`0x26aaa19`, repairs the surviving range's links, and changes its state to free
(zero) at `0x26aaa8a`.

There are immediate-free alternatives `0x26aa2e0` and `0x26aa440` without this completion
test. Direct callers include context destructor `0x26aec40` and cleanup `0x26af3f0`; the latter
is called from a context teardown path at `0x216830a` and initializer-error cleanup at
`0x26aefa4`. Virtual allocator entry points also expose these alternatives. Their callers'
complete synchronization preconditions are not established here. Thus the normal reclaimer
is not a proof that *every* free in the game is label-guarded.

### These nodes protect data read by the GPU

One concrete path reaches index-buffer binding:

1. `0x26d0d30` allocates through the resource allocator, stores the returned node in a
   resource lookup, and returns **node[+8]** as the payload address (`0x26d0f2a..0x26d0f36`).
2. `0x26d0ff0` looks that node up and likewise returns its payload address
   (`0x26d106a..0x26d1077`).
3. The binding path at `0x26bba89..0x26bbaa0` calls that lookup and adds the resource's
   index offset. At `0x26bbad2` it calls `0x10752c0` with the resulting address.
4. `0x10752c0` emits PM4 **INDEX_BASE**, header `0xc0012600`, and the payload address.

This is a resource-range allocator, not solely the separate command-chunk allocator
(`0x26d3b80`, whose chunk-status word follows a different 0/1/2 protocol). The 48-byte metadata
node itself need not be an index buffer: its completion allows reuse of the payload range it
describes. Submit-time command snapshots preserve the packet containing INDEX_BASE, not the
bytes later read through that address by Metal.

Renderer source review confirms the Apple binding side: `BindIndexBuffer` calls
`buffer_cache.ObtainBuffer(index_address, size, false)`, whose Apple path returns the imported
guest-memory arena. Vertex and storage/constant buffers use the same import path. The small
read-only constant ring is an exception: stage A copies those bytes before recording. Thus the
index-data lifetime chain above reaches actual zero-copy binding, not merely a hypothetical
renderer implementation. A clinic A/B with GPU-completion label writes is still needed to
attribute the observed intermittent geometry/colour corruption to this mechanism.

The native stream path copies small read-only buffers at recording time, but larger index and
vertex buffers can remain bound in place on Linux/Windows too. Their completion-label ordering
needs a separate audit; the small-buffer copy path does not establish native immunity.

### Consequences for removing the idle throttle

- Completion value 4 must mean that earlier GPU uses of the protected payload are complete,
  or that all such uses have stable renderer-owned copies. Merely recording their commands,
  or finishing host copies unrelated to those reads, does not establish this for zero-copy binds.
- Flip status must represent completion sufficient for the guest to reuse the display slot
  selected by the gate. Publishing an enqueue-only flip serial cannot substitute for that
  lifetime guarantee. This audit identifies the guest predicate, not a proof of the host
  present implementation or physical scanout timing.
- A check that detects label changes **before** an EOP write can catch premature direct frees.
  It cannot detect payload overwrite **after** an early completion write. Zero such reports
  therefore do not establish safe GPU data lifetime or absence of rendering corruption.
- A fixed extra frame of idle lookahead is not derived from this code. The normal node lifetime
  is completion-based, with a separate B-slot flip limit. B and actual node retirement/consumption
  times require live evidence if a numerical frames-in-flight limit is needed.

The historical `542f2d4` soak result remains evidence that draining deferred writes before
GpuIdle prevented a heap corruption. Its raw crash/write-log artifact was not available for
this audit. The recorded corrupt link `0x0000005300000000` is not the inline completion value 4
decoded above; the prose alone cannot identify which exact node/write path caused that crash,
nor prove that a SubmitDone return unconditionally authorized its free.

For a targeted follow-up, framed entry probes can cover `0x26d6e50` (RDI identifies F/B),
`0x26aa5a0`/`0x26aa620` (RDI allocator, RSI points to CB, RDX node),
`0x26aa860` (RDI allocator), and the immediate-free `0x26aa2e0`/`0x26aa440`
(RDI allocator, RSI node). The existing 64-byte entry windows do not include F/B or a payload
range's end through its neighbor; those need separately bounded safe reads. Avoid a global
node-pool allocation probe unless the specific allocator is filtered: that pool helper is shared.

## Label content check (`BB_LABEL_CHECK=1`, diagnostic)

The guest frees per-frame objects holding end-of-pipe labels (48-byte heap blocks, label at
offset 0) once the submitDone throttle releases it. A label written after that corrupts its heap
(guest fault 0x263b8e7, `docs/history/SESSION_2026-09-30.md`). With `BB_LABEL_CHECK=1`, each EOP
label is sampled when the EOP is parsed and again just before it is written. A change in between
that does not match one of our recorded EOP writes (WRITE_DATA, EOS and DMA writes are not
tracked) is logged as `LABEL-CHECK`, with the values, the 16-byte
aligned 48 bytes around the label, and the time since the parse sample. Every 4096 checks a summary
gives the checked, changed, unreadable, unmatched and mixed-width counts.

This is supporting evidence, not proof:
- A change need not be a guest free; another writer may have changed the memory.
- No change does not prove the lifetime is right. A free that leaves the value equal, ABA, or a
  change after the second sample are invisible.
- Guest memory is only read with non-faulting Mach reads; unreadable samples are counted and not
  compared.
- Slots where a 32-bit and a 64-bit EOP meet are counted as mixed and not compared.

Test: `label-check-test`.

`BB_SUBMIT_THROTTLE=none` (UNSAFE diagnostic) never holds the guest for GPU idle. It is validated
and announced at the first submission. With `BB_LABEL_CHECK=1` it shows whether label contents
start to change before their writes once the guest is not held, and how soon after the parse they
are checked. It may corrupt the guest heap, so it is not for play.

## Labels at GPU completion (`BB_LABELS_AT_GPU_COMPLETE=1`, the fix under A/B)

End-of-pipe labels (EOP, EOS, compute RELEASE_MEM) were written once the recording side had passed
the work before them and its guest-memory copies were done. That is before Metal executes the
work. Guest buffers bound zero-copy are read by the GPU at execution. The guest's label-governed
reclaimer frees nodes once their label reads 4, even mid-frame, and those nodes own GPU-read data
such as index buffers (node+8 is passed to INDEX_BASE; Astra-2, 50b01dc). So the guest could
overwrite memory that recorded but not yet executed draws still read.

With `BB_LABELS_AT_GPU_COMPLETE=1`:
- EOP, EOS and RELEASE_MEM labels, and their interrupts, are written only once the GPU has
  completed the command buffer holding everything recorded before them.
- Labels issued on the draw recording thread (stage B) are batched rather than submitted one by
  one. A submission per label made loading take more than 15 minutes (run B, PREVIEW-11).
  - The label's command buffer is submitted by the next submission. That submission signals
    exactly the label's tick, which SubmitExecution asserts.
  - Stage B makes that submission between packets, before the packet counts as consumed (so a
    stage A drain includes it): as soon as no further packet is published (idle), or once the
    oldest batched label has waited 0.5 ms.
  - Stage B also submits before it waits for labels itself (WaitDeferredSignals).
  - Labels issued on stage A (no draw pipe, or a compute RELEASE_MEM after a drain) are still
    submitted at once.
  - Nobody waiting on a label is left waiting for a submission that only comes at the frame's end.
- A dedicated label writer thread writes labels strictly in the order they were issued in the
  stream (`gpu/shim/bbport_label_sequencer.h`), across the graphics and compute queues and across
  deferred and direct recording. A label that becomes ready early waits for the earlier ones. Each
  is written after its submission's timeline tick. It is not the priority runner, whose other ops
  may wait for ticks submitted only at the frame's end.
- Labels are stored only through the memory's backing, which cannot fault. A faulting store on the
  writer could enter the write-fault handler while a recording thread waits for that very label.
  A label without a backing stops.
- At shutdown, labels not written are counted as done (nothing waits for them) and reported.
- WaitDeferredSignals, and with it GpuIdle, waits for them.
- A RELEASE_MEM GDS store is a GPU copy. It is recorded, the GPU completes it (Rasterizer::Finish),
  the labels before it land, and only then is the interrupt raised. The guest reads the destination
  through the buffer cache's GPU-modified tracking.
- WRITE_DATA and DMA_DATA are unchanged. The command processor executes them in stream order
  without waiting for earlier draws, so the guest cannot take them as "GPU done".

This is the exact fix candidate, kept toggleable for an A/B on corruption and FPS, and the
precondition for dropping the submit throttle.

It can be switched live with bit 59 of `BB_TOGGLE_FILE` (`BB_LABELS_AT_GPU_COMPLETE=1` keeps it on
for the whole run). The mode is read when each label is issued and fixed for that label. At a
switch, the first label in the new mode waits until every label issued before it was written, so
the two modes never reorder labels. The switch is logged as
`Labels at GPU completion: ON|OFF at <UTC time> (switched; ...)`. One run can therefore alternate
the modes, say every 60 s, and compare its halves under the same load, cache and location.
