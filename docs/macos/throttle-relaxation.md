# Relaxing the GpuIdle submit throttle (design, for review; no code yet)

The GpuIdle throttle's wait has been 70-96% of the frame at the clinic pose. This note:
1. states what the throttle does now;
2. lists what it still protects, assuming labels at GPU completion are the default
   (46fe3e6, `bb/macos-opus-pm4-labels-default`) and flips publish at GPU retirement
   (toggle bit 60, merged);
3. proposes the smallest rule that keeps that protection;
4. says how to measure it.

Facts come from the code (macos 9c2ead2) and Astra-2's guest lifetime audit
(docs/macos/gpu-idle.md, "Guest lifetime audit"). Hypotheses are marked as such.

## 1. What the throttle does now

- **`sceGnmSubmitDone`** sets `submission_lock` unless `Liverpool::IsGpuIdle()`.
- **Every submission waits for the lock to clear** (`sceGnmSubmitCommandBuffers`,
  `SubmitAndFlip`, the compute submits: `WaitGpuIdleToSubmit`), and so does the next
  `sceGnmSubmitDone`.
- **The GpuIdle interrupt clears the lock** (`ResetSubmissionLock`). `Liverpool::Process`
  raises it, under `submit_mutex` and only with no submission queued, after:
  1. the parser has consumed every queued submission;
  2. the draw recording thread (stage B) has drained;
  3. `WaitDeferredSignals()` has returned. That means every deferred label has been written.

So the guest's first submission of frame N+1 cannot start until frame N is fully processed.

**With labels at GPU completion (the new default), step 3 waits for Metal to complete the work.**
The labels are written by the GpuLabelWriter only after their submission's timeline tick. So
the throttle has become a **full GPU drain per frame**. Guest CPU time, our parse and record, and
Metal execution run back to back, never in parallel. The 70-96% wait is therefore our parse,
record and GPU time seen from the guest. It should be re-measured with bit 59 on to split it
(section 5).

## 2. What it still protects

What the guest can see of GPU progress:
- the labels it polls (EOP/EOS/RELEASE_MEM, WRITE_DATA);
- flip status and flip events;
- **the throttle itself**: a submission or SubmitDone that blocks until the previous frame is
  done.

`sceGnmAreSubmitsAllowed` is not called directly (audit: no caller of its wrapper 0x1089b60).
Removing the throttle removes only the third. So what matters is which guest actions rely on it.

| Guest reuse | What gates it (audit / evidence) | Safe without the throttle? |
| --- | --- | --- |
| 48-byte resource-range nodes (they own GPU-read data such as index buffers) | Normal reclaimer 0x26aa860, at frame end and on allocation failure. It frees only nodes whose completion label reads 4. | **Yes**, with labels at GPU completion: 4 is written only after the GPU work before it completes. |
| Display slot / per-frame data selected by `F % B` | Frame gate at 0x26d6e50: `flip_arg >= F + 1 - B` before SubmitAndFlip. | **Yes**, with flips at retire: flip_arg advances only once the frame's copy retired, which is after its draws (same scheduler). |
| Submitted command memory (DCB/CCB/IBs) rewritten by the guest | C1 (no throttle) logged 5600 "guest rewrote submitted command memory before it was parsed". One buffer was resubmitted every frame with a counter in dword 373. The guest evidently reuses some command memory about one frame later, without a label. | **Yes for the parser.** It reads the snapshot taken at submit (docs/macos/command-snapshots.md), and Metal never reads guest command memory. Leftover case: a WRITE_DATA/DMA_DATA into command memory of a submission not yet parsed, already a loud stop (docs/macos/command-snapshots.md). |
| Immediate frees without a completion test: 0x26aa2e0/0x26aa440, from context teardown 0x26aec40/0x26af3f0 | Preconditions not established (audit). | **Unknown.** These are teardown paths (loading, scene changes). If they free memory the GPU still reads, the throttle's full drain was hiding it. |
| The command-chunk allocator 0x26d3b80 (status 0/1/2) | Not audited. | **Unknown** for the chunk contents, which the snapshot covers. The status word protocol is not covered. |
| Anything the guest does on wall time or call order alone (e.g. the movie step's 20 ms) | None. | The throttle never protected these. The title-idle A/B/C runs collided with any throttle placement. |

Host-side reasons, not guest semantics:
- **Bounded run-ahead.** Snapshot memory, the draw-prep queue, recorder chunks, staging and
  input latency all grow with frames in flight.
- **The GpuIdle interrupt.** The guest never observes it directly; only `ResetSubmissionLock`
  uses it.

**So the throttle still protects the two unknown rows, and our host resources. Everything we
know the guest gates by labels or flips is protected by those once they are exact (bits 59/60).**

## 3. The rule

The guest's own frame gate already bounds how far it runs ahead: at most B frames past the last
retired flip. That is the PS4 mechanism. So the smallest change is to stop the throttle from
also waiting for GPU completion, and to keep a host bound.

**Rule R1: release at "recorded", not "completed".** GpuIdle, and with it the lock, is raised
once:
1. every queued submission is parsed;
2. stage B has drained;
3. every **recorder-side** deferred write has landed (host copies and RecorderFences).

Labels issued at GPU completion are **issued** by then (sequenced, their submissions flushed to
Metal). They are written later by the label writer, when the GPU completes them.
- Exactness argument: a guest that reuses only on labels or flips still waits for GPU
  completion through them. Only guest actions that relied on "the next submission returned, so
  the GPU is done" lose protection, which is the two unknown rows.
- What it buys: Metal executes frame N while the guest builds N+1 and we parse it.

**Rule R2: depth K.** The first submission after `SubmitDone(N+K-1)` waits for frame N to reach
"recorded" (K=1 is R1). This lets our parse and record of N overlap the guest's CPU work for
N+1..N+K-1. It needs per-frame accounting (section 6). K is bounded by B in practice: the guest
blocks on its own flip gate first.

**Rule R3: capacity only.** No guest-visible wait unless the host holds more than M unrecorded
frames or more than X MB of snapshots. PS4-like (a full ring). Only after R1/R2 show no
reuse-before-read.

Order: R1, then R2 with K=2, then R3. Each one is a live toggle, like bits 59/60, so one run can
alternate. The current full drain stays available as R0.

## 4. Evidence that would block a step

- **Reuse before GPU read.** Opus-Renderer's draw-data check (hash a draw's index, vertex and
  constant data at record time and at GPU execution; log mismatches) is the direct detector for
  the two unknown rows. **Required before R1 ships by default.** `BB_LABEL_CHECK` only catches a
  label holder freed before its write; it cannot see payload reuse after a correct write.
- **Teardown.** Entry probes on 0x26aa2e0/0x26aa440 (RDI allocator, RSI node) during loading
  and area changes, correlated with GPU-read-check mismatches and with the parse/GPU ticks in
  flight at the time. If those frees overlap GPU reads, R1 needs a teardown condition: hold the
  throttle (R0) across context teardown, detected by its entry.
- **Chunk status.** RE of 0x26d3b80's 0/1/2 protocol: what moves a chunk back to 0, and is it a
  label?
- **Detectors that must stay at zero:**
  - CMD-INTERLEAVE outside the known movie race (the handoff covers that);
  - parser stops;
  - BB_LABEL_CHECK reports;
  - draw-data check mismatches;
  - Playtest's corruption detector;
  - CMD-SNAPSHOT rewrites: expected, counted, and harmless by the table above, provided parse
    correctness holds.

## 5. Measurement

**Arms**, alternated live every 60 s (6+6 periods per pair), with bits 59 and 60 on throughout:
- R0 vs R1, at the clinic idle pose and on a short walking route;
- then R1 vs R2 (K=2);
- title idle with `BB_MOVIE_HANDOFF=1`, so the movie race doesn't confound.

**Per half:**
- FPS and frame-time p50/p95/p99 (`BB_FPS` and present timestamps);
- the throttle wait: Submit pacing, i.e. the in-submission and submitDone waits, count and ms;
- parse lag: queued ahead at submit, mean and max;
- **GPU busy %**: the sum of per-submission GPU time (MoltenVK per-submission timestamps only;
  in-buffer timestamps misattribute on M4) over wall time;
- frames in flight: the guest's F minus the retired flip_arg, sampled at each SubmitAndFlip;
- every detector in section 4.

**Before R1, one run with R0** splits today's 70-96% into parse+record and GPU tail. Instrument
the idle path with how long `WaitDeferredSignals` waits for completion labels after the drain.
That bounds R1's gain: at most the GPU tail per frame, while the guest is still serialized
behind our parse and record. R2's further gain is bounded by the guest's CPU time per frame.

**Success for a step:** higher FPS and GPU busy %, with every section 4 detector at zero, over
at least 20 min of clinic plus route, with Playtest's eye check per half.

## 6. Implementation sketch (after review)

- **R1:**
  - Split `WaitDeferredSignals` into the recorder-side signals (host copies, RecorderFences)
    and the completion labels, whose count the label writer already keeps.
  - The idle path waits for the first, and for the second only under R0. The completion
    labels' submissions must be flushed (`FlushLabels(true)`) before GpuIdle, so none waits
    for a later frame's submission.
  - At shutdown, drop and report unwritten labels as now.
  - Toggle: bit 56 is free on macos (SubmitDoneWaitsIdle was withdrawn), with
    `BB_SUBMIT_THROTTLE=recorded|idle` for the whole run.
- **R2 (per frame):** at `SubmitDone(N)` record frame N's last graphics submission number
  (`snapshot_submits`) and the recorder-side signals issued. Frame N is recorded once the
  parser has completed that submission, stage B has passed it, and those signals are done.
  `WaitGpuIdleToSubmit` then waits for frame `current - K + 1`'s ticket instead of the global
  lock. This means GpuIdle per frame rather than "queue empty", raised from the parser as each
  frame's last submission retires.
- **Unchanged:** snapshots at submit, label order (sequencer), flip order (publisher),
  `CheckCommandMemoryWrite`.
