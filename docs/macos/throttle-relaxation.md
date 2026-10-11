# Relaxing the GpuIdle submit throttle (design, for review; R0 diagnostic only, no R1 code yet)

The GpuIdle throttle's wait has been 70-96% of the frame at the clinic pose. This note:
1. states what the throttle does now;
2. lists what it still protects, assuming labels at GPU completion are the default
   (46fe3e6, `bb/macos-opus-pm4-labels-default`) and flips publish at GPU retirement
   (toggle bit 60, merged);
3. proposes a measurement (R0) and an **explicitly unsafe opt-in experiment** (R1), plus later
   steps;
4. says how to measure them.

Facts come from the code (macos 9c2ead2) and Astra-2's guest lifetime audit
(docs/macos/gpu-idle.md, "Guest lifetime audit"). Hypotheses are marked as such.

R1 stays opt-in and unsafe until Astra-2's RE closes the teardown and chunk-status unknowns
(section 2). Revision 3 folds in Sol-2's and Opus-Renderer's reviews of revisions 1 and 2.

## 1. What the throttle does now

- **`sceGnmSubmitDone`** sets `submission_lock` unless `Liverpool::IsGpuIdle()`.
  `sceGnmSubmitDone` is IsGpuIdle's only caller.
- **Every submission waits for the lock to clear** (`sceGnmSubmitCommandBuffers`,
  `SubmitAndFlip`, the compute submits: `WaitGpuIdleToSubmit`), and so does the next
  `sceGnmSubmitDone`.
- **The GpuIdle interrupt clears the lock.** Its only listener is `ResetSubmissionLock`.
  `Liverpool::Process` raises it, under `submit_mutex` and only with no submission queued,
  after:
  1. the parser has consumed every queued submission;
  2. the draw recording thread (stage B) has drained;
  3. `WaitDeferredSignals()` has returned.

**With labels at GPU completion (the new default), step 3 introduces a completion-label
retirement dependency.** GpuIdle waits until every completion label issued so far has been
written. The label writer writes each one only after its submission's timeline tick, so the GPU
has completed the work up to and including each label's submission.

This is **not necessarily a full drain**:
- GPU work after a frame's last label (its tail) is not waited for. A frame without any label is
  the extreme case.
- Guest CPU work before its next submitting call, and our parse/record of earlier submissions,
  can overlap Metal execution.

How much serialization this adds is for R0 to measure (section 5). The observed wait also
includes writer scheduling and host-copy waits, so it is not a rigorous bound on what relaxing
could gain.

## 2. What it still protects

What the guest can see of GPU progress:
- the labels it polls (EOP/EOS/RELEASE_MEM, WRITE_DATA);
- flip status and flip events;
- **the throttle itself**: a submission or SubmitDone that blocks until the previous frame is
  done.

`sceGnmAreSubmitsAllowed` is not called directly (audit: no caller of its wrapper 0x1089b60).

**The throttle blocks submissions, not guest CPU writes.** Even under R0 (today), between
`SubmitDone(N)` and frame N+1's first submission, the guest CPU writes N+1's data while Metal
may still be executing N. Examples are constants and the dynamic vertex ring around
0x7043..0x704b. Zero-copy binding makes those writes visible to the GPU.
- **R0 is not a safety baseline for the frame ring.** Whole-frame corruption from ring reuse can
  happen under R0. Labels and flips (bits 59/60) gate it, not the throttle.
- **R1 widens the guest's exposure by about one frame of CPU work** while GPU N runs: N+1's
  post-submit work and N+2's pre-submit work (Opus-Renderer).

| Guest reuse | What gates it (audit / evidence) | Without the throttle |
| --- | --- | --- |
| 48-byte resource-range nodes (they own GPU-read data such as index buffers) | Normal reclaimer 0x26aa860, at frame end and on allocation failure. It frees only nodes whose completion label reads 4. For an external label (node[0] = pointer \| 1), that label's producer must also be GPU-complete. | Gated, **for the audited path**, by labels at GPU completion. External-label producers are not yet audited. |
| Display slot and per-frame data selected by `F % B` | Frame gate at 0x26d6e50: `flip_arg >= F + 1 - B` before SubmitAndFlip. | Bounds the **audited frame-end path** with flips at retire. It does not prove every payload write or allocation happens after the gate, and it does not bound compute (ASC) or non-flip submissions. |
| Submitted command memory (DCB/CCB/IBs) rewritten by the guest | C1 (no throttle) logged 5600 "guest rewrote submitted command memory before it was parsed": one per-frame buffer with a counter in dword 373. | **Packet bytes only:** the parser reads the snapshot taken at submit (docs/macos/command-snapshots.md). Metal never reads guest command memory as packets, and a WRITE_DATA/DMA_DATA into command memory not yet parsed is a loud stop. Labels or other GPU-readable data placed in command chunks (e.g. the chunk status word) are not covered by the snapshot and need their own completion lifetime (Astra-2). |
| Immediate range frees 0x26aa2e0/0x26aa440 (identical; also vtable slot +0x28 of 0x535f350/0x535f300), from context destructor 0x26aec40 and cleanup 0x26af3f0 | docs/macos/throttle-guest-lifetimes.md (Astra-2): allocator lock only, no GPU completion predicate. Some callers are init rollback or temporary-context destruction. 0x2196e10 queues retirement (0x26af700) before destroying contexts, but queueing is not waiting. | **Conditional on caller ownership/retirement**, not established for all (indirect) callers. A destructor hit alone is not a live unsafe free. |
| Command-chunk allocator 0x26d3b80 | Claim: `lock cmpxchg` qword 0 to 1 at the chunk's last qword. Release: CPU stores 2, then EOP (0x26d3c80) or RELEASE_MEM (0x26d3cb0) writes 0 at completion. Exception: cleanup's direct CPU zero 0x26d3d30 (from 0x26af3f0, 0x26b12f0), and pool teardown 0x26d3b50 (Astra-2). | **Ordinary reuse is completion-governed** (labels at GPU completion). The direct zero is not: if cleanup zeroes a status while an old release is pending, a new claimant can take the chunk and the old packet then zeroes its live status. The EOP release is visible to `BB_LABEL_CHECK` (status 1 found where 2 was parsed); the RELEASE_MEM release is not (the check covers EOP only). |
| Anything done on wall time or call order alone (e.g. the movie step's 20 ms) | None. | The throttle never protected these (title-idle arms A/B/C). |

### Run C: the one direct observation without the throttle

PREVIEW-11 run C (`20261011-075756-1080p-PM4-C-P11`, `BB_SUBMIT_THROTTLE=none`, labels at GPU
completion, flips **not** at retire) reached the world and then faulted in GetProgram's SRT
candidate walk: a null user-data table pointer s[2:3] (docs/macos/srt-candidate-faults.md).
The doc leaves two explanations open:
- **an incompatible candidate layout.** c6a8b7d now rejects that candidate without faulting,
  logs it (first 8 per thread, then every 1024), and falls back to a fresh translation;
- **live user data the guest had already reused.**

That walk reads **guest memory at parse/record time**: user-data tables, SRTs and shader code.
Throttle off lets the guest run ahead of our parse. So whatever per-frame memory those reads
come from can change before we read it. Run C had no flip gate at retire, so the guest's slot
reuse waited only for an enqueue-time flip.

What this means per step:
- **R1 keeps parse-time reads protected.** Its GpuIdle still requires frame N parsed and
  recorded before release, as R0 does. Only GPU-execution-time reads (zero-copy) are exposed
  further. So R1 alone should not reproduce run C's exposure.
- **R2, R3 and `none` expose parse-time reads.** The guest can be ahead of our parse, so run C
  is direct evidence for them, not for R1.
- **Bit 60 may change it.** With flips at retire, flip-gated per-frame data is reused only after
  the frame's GPU retirement, which is after its parse.
- **Before R2:** re-run run C's configuration with bits 59 and 60 (throttle `none`, as a
  diagnostic only) and count, against the same route with the throttle on:
  - SRT candidate rejections ("rejected on this thread");
  - fresh translations;
  - faults.

  More rejections without the throttle, even with no fault, would mean stale parse-time reads,
  which the c6a8b7d guard turns from a crash into a possibly wrong render.

Host-side reasons, not guest semantics:
- **Bounded run-ahead.** Snapshot bytes, the draw-prep queue, recorder chunks, staging and
  input latency all grow with frames in flight.

## 3. Steps

### R0: the wait split (diagnostic, no behaviour change)

R0 keeps the throttle as it is and splits each GpuIdle into:
- the parse;
- the stage-B drain;
- the recorder-side deferred writes (host copies, RecorderFences);
- the completion labels.

It also records whether the last GPU submission before GpuIdle came after the last completion
label (a tail that is not waited for), and by how many ticks.
- **Output:** one stdout line every 600 frames.
- **First run:** at the clinic, once coreaudiod is healthy (this is a perf measurement).

### R1: release at "recorded" (opt-in, UNSAFE experiment)

**Frame tickets.** Each frame gets a ticket at the guest's `sceGnmSubmitDone`: a boundary placed
after the last submission admitted before it. The ticket is **sealed by the GPU thread** when
it crosses that boundary, not at the guest call, whose PM4 may still be unparsed. It is sealed
once:
1. the parser has completed every submission before the boundary, graphics and compute (ASC),
   in all participating queues;
2. stage B has passed it;
3. the recorder has flushed it to Metal.

When sealed, the ticket records:
- **the GPU timeline tick** of the frame's last submission, the **end-of-frame tick**. Its
  completion means the GPU has completed every bit of work the frame submitted, with or
  without labels, including the tail after its last label.
- the **recorder-side prefix** (`recorder_issued`);
- the **label prefix** (`labels_issued`).

All three are issued counts reached at the boundary.

ASC: compute submissions belong to the frame whose boundary follows them. If the parser cannot
attribute an ASC submission to a frame (for example a compute queue still running across the
boundary), R1 refuses it: a loud log, and that frame falls back to R0.

**Admission.** Frame N's GpuIdle is raised under `submit_mutex` with no submission queued (the
existing race protection), once N's ticket is sealed and:
1. the recorder-side writes up to its prefix have completed;
2. every completion label up to its label prefix is **issued and flushed** to Metal. It is not
   necessarily written. At GpuIdle we assert that no stage-B batched label is unflushed
   (`labels_unflushed` false), rather than relying on the flush order: a batched label must
   never wait for a submission the throttle holds back.

The labels remain guest-visible obligations of their own, written by the label writer at their
ticks.

**Host cap (enforced, part of R1).** The submission after `SubmitDone(N)` also waits until:
- **frame N-1's end-of-frame tick is free** (`IsFree`), so at most two frames are outstanding on
  the GPU, labels or not (Opus-Renderer, Sol-2);
- the snapshot budget allows it:
  - a submission **reserves** its snapshot bytes before copying, and releases them when parsed;
  - it waits while reserved bytes plus its own exceed 64 MiB (a start, to be set from R0);
  - a single submission larger than the whole budget is admitted alone, once nothing else is
    reserved, and counted.

**Accounting.** Two separate monotonic pairs:
- `recorder_issued`/`recorder_done`;
- `labels_issued`/`labels_written`.

In-flight callbacks count as issued until they complete. Today's single
`deferred_signals_issued/done` remains their sum. R0 (071cfc6) already keeps the label pair.

**Live switching** (a new toggle bit, 56, and `BB_SUBMIT_THROTTLE=recorded|idle` for the whole
run):
- **Latched at a frame boundary** (the ticket), never mid-frame.
- **R1 to R0:** before raising the first R0 GpuIdle:
  - drain through the end-of-frame ticks of every frame admitted under R1;
  - complete their recorder-side writes;
  - write their labels.

  That is a full fence through all previously admitted work, not only the labels.
- **R0 to R1:** pending obligations carry over. R1 is refused (loud log, stays R0) unless labels
  at GPU completion and flips at retire are both on at that boundary.
- **Turning bit 59 or 60 off while R1 is active** forces the R1-to-R0 transition at the next
  boundary, before the change takes effect for frames admitted after it. The prerequisites are
  checked at every boundary, not only once.
- **A/B statistics** exclude the frame containing a switch and the fence or drain it causes.

**Teardown.** An R0 switch at the entry of a teardown free would be too late: the freeing thread
may already be past the submit throttle. Real protection has to:
- block that free before it changes any storage;
- wait for the GPU reads issued before it;
- come with a lock-order proof. The freeing thread may hold the guest allocator lock while
  waiting, and GPU progress must not need that lock or a CPU write from that thread (WAIT_REG_MEM
  on CPU-written memory).

Until Astra-2's RE establishes the callers' preconditions, the teardown paths remain a stated risk
of the R1 experiment, and R1 is not a default candidate.

### R2: depth K (later)

The first submission after `SubmitDone(N+K-1)` waits for frame N's admission. Tickets must
cover:
- **every queue SubmitDone covers** (graphics and ASC): `snapshot_submits` counts graphics only;
- **exact stage-B and recorder prefixes** up to frame N's last submission.

Changing GpuIdle from "queue empty" to per-frame completion must be shown equivalent for
`ResetSubmissionLock` and `IsGpuIdle`, the only listener and caller today.

### R3: capacity only (later)

Only after R1/R2 evidence and the lifetime RE.

## 4. Evidence

**Zero-error list.** Each of these must stay at zero:
- CMD-INTERLEAVE outside the known movie race (the attract video at title idle; the handoff
  that was meant to remove it is falsified and removed, docs/macos/movie-handoff.md);
- parser stops and guest faults (run C's GetProgram/SRT-walker fault in particular);
- `BB_LABEL_CHECK` reports;
- Playtest's corruption detector.

**Must not rise over R0 at the same pose:** SRT candidate rejections (c6a8b7d's log). Under R1
these should be unchanged (section 2, run C).

**Expected and counted, not errors:** CMD-SNAPSHOT guest rewrites. The snapshots make them
harmless to the parse (section 2). Report their rate per half; a change between halves is
informative.

**Supporting evidence, not proof: Opus-Renderer's `BB_GPU_READ_CHECK`.**
- **What it does:** it hashes each draw's read-only buffer ranges at record time and again after
  the GPU has retired the work.
  - `=1`: up to 64 KB, plus index ranges.
  - `=2`: up to 1 MB, including vertex buffers. Use `=2` on the gate runs.
- **What it misses:**
  - written ranges, and ranges marked GPU-modified at record;
  - buffers over 1 MB, for example whole vertex pools of about 3.8 MB;
  - pointer and BDA reads;
  - textures. The texture cache copies guest memory into images at record. That protects GPU
    reads only after the copy has completed: it does not show that the guest could not reuse
    the memory before a deferred copy ran. So textures are unobserved by this checker.
- **Why it can be wrong either way:** a safe reuse after retirement can mismatch, and a
  mutate-then-restore can match.
- **How to use it:** compare its counts, together with its coverage and drop counters, between
  R0 and R1 halves at the same pose. An R1 increase is the signal.
- **What it cannot do:** zero reports cannot discharge the teardown or chunk-status rows.

**Default-on requires** the lifetime preconditions of section 2 to be established, or an
enforceable protection to be retained for each unknown row.

**Teardown probes.** Run entry probes on 0x26aa2e0/0x26aa440/0x26d3d30 during loading and area
changes, in the same run as `BB_GPU_READ_CHECK=2`. That run correlates the probes with read-check
changes on that context's ranges, and with the GPU ticks in flight at the time.

## 5. Measurement

**Arms**, alternated live every 60 s (6+6 periods), with bits 59 and 60 on throughout:
- R0 vs R1, at the clinic idle pose and on a short walking route;
- title idle, counting the movie race's CMD-INTERLEAVE separately.

**Per half:**
- FPS and frame-time p50/p95/p99;
- the throttle wait: Submit pacing, i.e. the in-submission and submitDone waits;
- R0's split, and the tail-after-last-label count and ticks;
- parse lag: queued ahead at submit, mean and max;
- **GPU busy %**: the sum of per-submission GPU time over wall time. MoltenVK per-submission
  timestamps only; in-buffer timestamps misattribute on M4;
- **submissions per frame**. Label batching and R1 change it, and each submission carries a
  fixed GPU overhead;
- frames in flight: the guest's F minus the retired flip_arg, sampled at each SubmitAndFlip;
- R1's host-cap waits (count and ms) and the queued snapshot bytes;
- every section 4 counter.

**Success for R1 as an experiment:**
- higher FPS and GPU busy %;
- the zero-error list at zero;
- no read-check increase over R0 at the same pose;
- over at least 20 min of clinic plus route, with Playtest's eye check per half.

Default-on additionally needs section 4's lifetime condition.
