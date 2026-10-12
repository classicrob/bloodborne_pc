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
that does not match one of our tracked label writes (EOP, and RELEASE_MEM with 32/64-bit data) (WRITE_DATA, EOS and DMA writes are not
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

## Labels at GPU completion (the default on Apple)

End-of-pipe labels (EOP, EOS, compute RELEASE_MEM) were written once the recording side had passed
the work before them and its guest-memory copies were done. That is before Metal executes the
work. Guest buffers bound zero-copy are read by the GPU at execution. The guest's label-governed
reclaimer frees nodes once their label reads 4, even mid-frame, and those nodes own GPU-read data
such as index buffers (node+8 is passed to INDEX_BASE; Astra-2, 50b01dc). So the guest could
overwrite memory that recorded but not yet executed draws still read.

Labels are therefore written at GPU completion. This is the default since labels at completion
removed the corrupted frames (lighting floods, frame-to-frame alternation) on PREVIEW-17:
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

It remains switchable for diagnosis (`gpu/shim/bbport_label_mode.h`):
- Bit 59 of `BB_TOGGLE_FILE` (`LabelsAtSubmit`, live) writes labels as soon as the work before
  them is recorded, the old behaviour.
- `BB_LABELS_AT_GPU_COMPLETE=0` does the same for the whole run, and `=1` keeps labels at GPU
  completion for the whole run. Either ignores bit 59. Any other value stops.
- The startup line `Labels (EOP/EOS/RELEASE_MEM): ...` names the mode.

Note the inversion: before this became the default, bit 59 set meant labels at GPU
completion. Masks from those A/Bs now mean the opposite.

The mode is read when each label is issued and fixed for that label. At a
switch, the first label in the new mode waits until every label issued before it was written, so
the two modes never reorder labels. The switch is logged as
`Labels at GPU completion: ON|OFF at <UTC time> (switched; ...)`. One run can therefore alternate
the modes, say every 60 s, and compare its halves under the same load, cache and location.

### In-order WRITE_DATA: ME-time order (default; toggle bit 63 restores the old wait)

Before this change, a WRITE_DATA run in order on the draw recording thread (RunWriteData) first
waited for **every earlier deferred signal**. With labels at GPU completion that is every earlier
EOP/EOS/RELEASE_MEM label, i.e. GPU completion. That rule is stricter than the PS4: there the CP
writes WRITE_DATA when it parses it, ahead of the pipeline, and earlier end-of-pipe labels may land
after it. In the async A/B the stall was 20-34 ms per frame of stage B.

Now, with labels at GPU completion, a WRITE_DATA waits for what is ordered before it at ME time:
- the host copies (as before);
- the recorder-side writes (WaitRecorderSignals);
- **and the labels behind any WAIT_REG_MEM that stage A met early** from a pending fence value
  (PendingFenceValue). On the PS4 that WAIT would have blocked the CP until the label landed, so a
  later WRITE_DATA must not pass it. The guest has such pairs (EOS/EOP L=1 then WAIT_REG_MEM L==1,
  0x26b08e0 / 0x26b09e0, Astra-2's deferred-signal audit).
  - The dependency is taken **at the WAIT's position in the stream**: stage A puts an in-order
    marker there, which runs after the producing EOP/EOS has issued its label. A value sampled on
    stage A at parse could predate that label (Astra-2, Sol-2).
  - It counts **completion labels only**. Their one writer completes them in issue order, so "N
    written" is a prefix. The mixed deferred-signal count is not a prefix: a later recorder write
    can complete first.
  - The barrier belongs to the scheduler (gpu/shim/bbport_write_order.h) and only rises, so a
    WRITE_DATA may wait for a later barrier than its own position needed: longer, never shorter.
  - Stage B submits its batched labels before waiting.
  - Tests (test_label_check, through a real draw pipe): stage B held while EOP, the marker, a
    later recorder write and WRITE_DATA are committed. The recorder write completes first and the
    label stays pending, and WRITE_DATA waits. Without a WAIT it does not. A negative control
    shows that a marker taken before its producer lets WRITE_DATA through.

A WRITE_DATA before a later completion label stays visible first: it is written when stage B runs
it, the label only at GPU completion. Toggle bit 63 (WriteDataAfterLabels) restores the old wait
live, for the A/B; with labels at submit (bit 59) nothing changes. The parse split charges these
waits to their own kinds, "deferred signals before WRITE_DATA" and "deferred signals before a flip".

### Flush at a WAIT_REG_MEM met early (always)

The waits that remain for an in-order WRITE_DATA are required: they are the labels behind a
WAIT_REG_MEM the CP would have stalled at. Their cost is label latency (issue to write: about
10 ms at the clinic idle). When stage B runs a met-early WAIT's marker, it submits its batched
labels at once ("label batch at a WAIT met early" in the census), so the producing label's work
reaches the GPU without waiting for the batch age or the next idle. This is exact: an earlier
submission, never an earlier write. The Flushes line counts these WAITs ("WAIT_REG_MEMs met
early N per frame").

### Later label flushing (toggle bit 56, an A/B; off by default)

Stage B batches the completion labels it issues and submits them at the next packet boundary
where the draw pipe is momentarily idle, or once the oldest has waited 500 us. Each submission
ends the open command buffer: a render pass split and a recorder sync. With async submission
those idle flushes went from about 5 to 22 per frame.

With bit 56 set, a momentary idle no longer flushes. The batch is submitted:
- at LabelBatchAge or frame end, as before;
- when stage B is about to sleep, after its spin of about 200 us with no packet (the draw pipe's
  idle hook). This is the backstop: whenever stage B stops working, its labels go out within its
  spin time, so a guest or stage A waiting for one never waits on an unsubmitted batch for
  longer than that;
- when a thread about to wait for labels asks. Stage A's WaitDeferredSignals and WaitLabelsThrough
  set a request that the spinning stage B serves within 256 spins.

Labels are still written only at GPU completion of a submission that contains their work: later,
never earlier. The census reasons are "label batch before sleep" and "label batch asked for".
Renderer's proposed "no idle flush while a submission is queued" is covered: no idle flush at all,
with the sleep backstop, in the async and sync builds alike. Read the A/B against the Flushes
line, the label age at write, and the render pass census.

## Flip publication at GPU retirement (the default on Apple)

A flip tells the guest that its previous display buffer may be reused. That means the flip
status (count, flip_arg, current buffer, pending counters), the flip events, and the previous
buffer's label reset to 0. The guest's frame gate (`FlipStatus.flip_arg >= F + 1 - B`) and its
display-buffer reuse depend on it.

The presenter reads the guest framebuffer only in the frame copy that PrepareFrame records and
submits on the draw scheduler (`Frame::ready_tick`). Presenting to the swapchain reads our copy,
not guest memory. So that copy's GPU retirement is the exact point. The moment the frame is
actually on screen matters only for pacing, which vblank events model, and MoltenVK does not
report it reliably.

Flips are published at that point by default (`gpu/shim/bbport_flip_mode.h`). For diagnosis:
- Bit 60 of `BB_TOGGLE_FILE` (`FlipAtEnqueue`, live) publishes them when enqueued, the old
  behaviour.
- `BB_FLIP_AT_GPU_RETIRE=0` does the same for the whole run, and `=1` keeps them at retirement
  for the whole run. Either ignores bit 60. Any other value stops.
- The startup line `Flips: ...` names the mode.

Note the inversion: before this became the default, bit 60 set meant publishing at
retirement. Masks from those A/Bs now mean the opposite.

`BbFlip::Publisher` (`gpu/shim/bbport_flip_publish.h`, test `flip-publish-test`) does the
publishing:
- Each flip carries its frame copy's ready_tick, read in SubmitFlipInternal while the frame is
  still exclusively ours. A blank frame carries its own copy submission's tick.
- It also carries the port generation, taken in SubmitFlip together with the pending count.
- The publisher's thread waits for each tick in flip order, then publishes.
- In the enqueue mode, flips publish at once, unless publications are still queued. Then they
  queue behind them, so order holds across a live switch.
- The port's flip lifecycle is shared code (`core/libraries/videoout/port_lifecycle.h`, test
  `videoout-lifecycle-test`):
  - A flip is admitted only to an open port and a registered buffer, together with its pending
    count and generation. Otherwise the guest gets its error (invalid handle, invalid index) and
    nothing is counted. An EOP flip that reaches a closed port is dropped and reported. A buffer
    unregistered while its flip is pending stops the run, as on the other platforms.
  - Close resets the port and bumps the generation in one step under port_mutex, and holds the
    lifecycle lock across its whole reset. A publication holds the same lock for its check and
    all three effects.
  - A flip admitted before a Close is dropped before its frame is prepared (it is never read
    from the reset or reopened port's buffers) and again at publication (nothing decremented,
    no buffer released). Both are reported.
- Flips still queued at shutdown are reported.
