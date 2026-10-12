# Asynchronous submission (Apple)

Design passed by Sol-2 (code contract) and PM4 (labels, flips, timeline); implemented in vk_scheduler.cpp (EnqueueSubmission, SubmitFromRecorder, WaitQueueSubmitted, WaitDependencies), tested by scheduler-async-submit-test. Not yet qualified in game (the A/B below).

## Problem (R0 parse split, PREVIEW-20 idle, about 88 ms per frame)

On stage B, per frame:
- "queue submit" (`vkQueueSubmit`, inside which MoltenVK encodes the command buffer into
  Metal) costs 27-35 ms;
- "recorder sync" (every `Flush` waits for the Vulkan recording thread to run dry) costs
  7-12 ms.

That is over 48-80 submissions. Prefill showed that only moving the encoding does not help while
Flush still waits for it.

**Unvalidated model, not an expected outcome:** if stage B stopped paying both, the idle frame
could drop toward max(stage A, stage B, recording thread). The measured sums overlap other waits.
Only the A/B decides the gain.

## Design

`Flush(info)` on the owner thread (stage B for the draw scheduler) keeps everything that is
owner-state:
- `on_submit` (the rasterizer's pending barriers, recorded into this submission);
- EndRendering;
- tick assignment: `signal_value = NextTick()`. The batched-labels assert
  `signal_value == unflushed_tick` stays here. PM4 invariant 1 holds.
- `PopPendingOperations`.

Instead of syncing, ending, submitting and beginning, Flush appends one **owned submit packet**
to the record stream and returns. The packet holds:
- **copies**, not references: the SubmitInfo wait and signal semaphore arrays with their values
  and stages, and the fence;
- `next_submit_waits`, **moved** into the packet at enqueue (MetalFX's waits belong to this
  submission);
- `signal_value`, and the **next** submission's tick `signal_value + 1`, which the begin uses for
  the pool (see Bounds);
- the perf state: phase, frame and the query slots this submission ends in;
- `copies_through`, the host-copy prefix this submission reads (see Host copies).

The recording thread executes the stream in FIFO order. At a submit packet it:
1. ends the command buffer it has been recording (recorder-owned since the previous packet's
   begin);
2. waits for the sealed host-copy prefix;
3. calls `vkQueueSubmit` under `submit_mutex` with the packet's arrays;
4. publishes `submitted_through = signal_value` and wakes WaitSubmitted waiters;
5. commits the next command buffer from the pool, with the packet's next tick, and begins it
   (perf Begin with the packet's phase and frame).

The recording thread never reads stage B's mutable state (`current tick`, perf phase,
`next_submit_waits`), only the packet.

**Accessor audit** (each must run on the thread that owns the command buffer):
- `CommandBuffer()` (direct recording): SyncRecording first, which now also drains pending submit
  and begin packets. The command buffer it returns is then the recorder's freshly begun one, and
  the caller records on it while the recorder is idle. That is the existing direct-mode contract.
- `on_submit`: owner thread, before the packet. It records through `Record()`.
- `ImGui::Core::TextureManager::Submit()`: a no-op in our build
  (gpu/shim/imgui/renderer/texture_manager.h). Dropped from the path; if it ever does work, it
  must move into the packet.
- **Pool, query pool, perf:** recorder only, from the packet. Stage B never touches them after
  enqueue.

## Host copies (Sol-2 point 1; PM4 condition ii)

Today `WaitHostCopies` rereads `host_copies_issued`, kicks recording, and waits for callbacks that
run on the recording thread. On the recording thread that would wait on itself. Instead:
- At enqueue, Flush **seals** the prefix: `copies_through = host_copies_issued`. The copy
  dispatch callbacks for those copies are recorded before the packet (FIFO). So when the packet
  executes, they have run, and the recording thread waits **only for copy-worker completion of
  copies <= copies_through**. It never waits for later copies, and never for its own callbacks.
- **PM4's ordering (i):** stage B's deferred labels (`labels.Ready` via `BbCopy::AfterCopies`)
  are lambdas recorded before the packet, so they run on the recording thread before their own
  submission, as today. The unbatched stage A path (`Flush(); WaitHostCopies(); labels.Ready(seq,
  tick)`) stays on the owner. Its WaitHostCopies waits for copy completion only (no kick of
  itself), and the label thread waits for the tick anyway.
- **PM4's guard (ii), a loud stop:** a host copy awaited at packet V must not depend on a GPU
  tick >= V (a download of this or a later submission). Each copy records the tick it waits on;
  the recorder checks it before waiting.
- **Flush's enqueue always wakes the recording thread** (KickRecording(true) semantics), so the
  copy-dispatch callbacks recorded before the packet run. Stage A's unbatched-label
  WaitHostCopies, which no longer kicks, relies on this (PM4).
- **Controls:**
  - an earlier copy is held and a later copy is issued after the enqueue: the packet waits for
    the earlier one only. A sleeping recording thread is woken by the enqueue alone, with nothing
    else kicking;
  - a copy depending on its own submission's tick stops.

## Waits and dependency graph (Sol-2 point 3)

Kinds of semaphore waits in our submissions:
- **Scheduler-owned timeline waits:** present and flip submissions wait on the draw scheduler's
  timeline value V (frame copies, flips at retire). Before building such a SubmitInfo, the waiting
  scheduler calls `draw.WaitSubmitted(V)` **outside `submit_mutex`**. It is a condition-variable
  wait on `submitted_through >= V`.
- **Binary swapchain acquire semaphores:** signalled by the presentation engine and not tied to
  our submit queue order. Unchanged.
- **External MetalFX waits** (`WaitBeforeNextSubmit`): signalled by MetalFX's Metal queue after
  its own commit, which depends on an earlier draw submission. FIFO submits that one first.

**Acyclic:** the only cross-scheduler edges point from present/flip to draw. Draw never waits on
present/flip timelines (to verify by grep before coding, and assert at enqueue).

**Stops, each loud:**
- a submission waiting on its **own** scheduler's value >= its signal (self or future wait) stops
  at enqueue;
- a WaitSubmitted for a value the scheduler has not even assigned (> CurrentTick) stops;
- on submit failure or Close, WaitSubmitted waiters are woken with a failure state, and the
  waiter stops.

**Controls:**
- a delayed producer: the draw scheduler's recording thread held while the present scheduler
  submits a wait on draw V;
- a self-wait stop;
- a waiter woken by shutdown.

## Bounds and lifetime (Sol-2 point 4)

- **Queued packets:** at most 8 unsubmitted. Flush blocks beyond that, in its own split scope.
  Chunk bytes are bounded by the existing chunk pool (bounded pool; Flush waits for a free chunk,
  stated).
- **Command buffers and query slots pending on the GPU:** a command buffer is reusable when its
  tick is free.
  - Today `CommitResource` stamps a buffer with `CurrentTick()`, read at commit on the owner.
    Under this design the recorder commits with the **packet's explicit next tick**, so stamping
    does not read owner state. `ResourcePool::CommitResource(tick)` gets that parameter.
  - The total is bounded by GPU depth plus the 8 queued. The existing pool grows on demand;
    that stays, with a counter and a stop above a hard cap (e.g. 256) so a leak is loud.
  - BB_PERF query slots: the packet carries its slot. The ring of 512 is checked for wrap
    against the oldest unretired slot, and stops if it would overwrite.
- **Shutdown:** `Close` (the tested contract from bb/opus-renderer-scheduler-close, sent for
  merge separately) becomes:
  1. drain the record stream, including all queued packets, so each is submitted;
  2. then the final submission;
  3. wait for its tick;
  4. seal.
  It runs in Presenter's destructor before the rasterizer, caches and callback owners die. The
  recording thread is joined by `~Scheduler` after Close. This branch depends on that one.

## Labels-at-submit (Sol-2 point 5)

With bit 59 (LabelsAtSubmit, diagnosis), "at submit" now fires at **Flush**, before the actual
vkQueueSubmit. That is earlier than today and a **changed, unsafe diagnostic**. The doc and the
startup line will say so. Completion labels, the default, are published after their assigned
tick as today.

## Measurement (with the A/B)

- the parse split: stage B "queue submit" and "recorder sync", and the recording thread;
- PM4's Flush census;
- **label age at write** (issue to written). It must not grow by more than about one
  submission's encode time, or the guest's reclaimer and the GpuIdle throttle see later labels.
- frame time and FPS, and shots.

## Tests

scheduler-async-submit-test (device):
- FIFO order over 1000 submissions with fills and timeline checks;
- `Wait(tick)` on a queued tick;
- Finish;
- interleaved direct recording;
- the packet cap;
- the cross-scheduler delayed producer;
- `Close` with queued packets;
- the host-copy prefix (held earlier copy, later issue);
- negatives, each a stop with exit 23 and its message: a self/future wait, a copy depending on its
  own tick, a wait on the recording thread, a pool above the cap.

## Implementation requirements (Sol-2), as built

1. **Chunk budget.** The chunk pool is NOT bounded today: AcquireChunk allocates when
   free_chunks is empty, and KickRecording appends without limit. Add a real chunk/byte budget
   counting queued chunks, full chunks the producer holds, and the executing chunk, with
   backpressure that lets the recording thread progress. Eight packets alone do not bound a long
   submission.
2. **WaitSubmitted(V) rejects V >= CurrentTick().** CurrentTick is the open, next tick; NextTick
   returns the old value and then increments. Publish an enqueued watermark, so a reserved but
   never-enqueued tick is never mistaken for a submit-ready dependency. Test the boundary
   (V == CurrentTick() - 1 versus == CurrentTick()).
3. **The submit packet is a strict chunk boundary.** RecordChunk::Execute takes the command
   buffer by value for the whole chunk, so the packet must be its own queue item or end a chunk,
   and commands after it get the newly begun buffer. Control: consecutive submissions small
   enough to share a chunk, with distinct fills and tick checks.
4. **The copy prefix is a contiguous completion fence** (the ordered AfterCopies epochs), not a
   count that later-completing copies could satisfy. Keep the control with an earlier copy held
   and a later one issued.
5. **After a successful Close,** WaitSubmitted for an already-submitted V still succeeds; only
   unsatisfied or cancelled dependencies fail.
6. **Apple only.** SubmitPendingArenaBinds does a real bindSparse on other platforms under the
   old submit mutex, so the non-Apple path stays as it is.
7. **Keep the old WaitSubmitted() semantics** (wait for GPU retirement of the previous
   submission; ExtraSharpen's resource destruction relies on it) under its new name.

As built:
- the chunk budget is 128 chunks (16 MiB), counted when allocated, with AcquireChunk handing the
  owner's full chunks to the recorder and waiting for one to come back;
- WaitQueueSubmitted stops for V >= CurrentTick() and after 10 s; a value is enqueued in the same
  call that assigns it;
- the packet is its own recorder queue item;
- the copy prefix is checked as dispatched (host_copies_done >= copies_through, FIFO) and then
  waited for through BbCopy::AfterCopies, a 2 s stop instead of a silent hang;
- after Close a submitted value still satisfies WaitQueueSubmitted;
- non-Apple keeps the synchronous path, and WaitSubmitted() is now WaitPreviousRetired()
  (fsr4, temporal upscaler).

## Host copies: audit (PM4's condition ii)

Every host-copy task copies guest memory into staging and flushes it; none waits on a GPU
submission:
- buffer_cache.cpp SmallGuestCopy (RecordHostCopy on the recording thread, or a BbCopy batch);
- ObtainBufferForImage's chunks (BbCopy::Async);
- the deferred upload groups (BbCopy::Async);
- ParallelFor (synchronous on the caller).

So the copy wait at a packet cannot form a cycle with a submission. Its 10 s limit is a hang
detector (copy threads run at background priority, and loaded builds slow them), not a
dependency check.

Copies the owner thread issues with BbCopy::Async after enqueueing a packet (ObtainBuffer for
later submissions) are also covered by the packet's AfterCopies wait. That is an over-wait,
correct but not the minimal prefix. Only copies dispatched by recorded commands (RecordHostCopy)
are strictly ordered by the stream.

## Source review round 2 (Sol-2), as built

- **Sealing before ordering.** `SealSubmission` assigns the value and runs the labels check,
  `on_submit` and the merge of WaitBeforeNextSubmit's waits. Then `WaitDependencies(info, value)`
  runs on the complete wait list, outside submit_mutex, in every path: asynchronous enqueue,
  synchronous fallback, and Close. On Apple, `SubmitExecution(info, value)` takes the sealed
  value and does not seal again. Non-Apple is unchanged.
- **The synchronous fallback drains first.** Direct recording, a live ThreadedRecording toggle
  and Close call `SyncRecording` before `SubmitExecution` takes submit_mutex, because queued
  packets need that lock to be submitted.
- **Contract narrowed to proven edges.** WaitDependencies stops when an asynchronously submitting
  scheduler waits on another asynchronous timeline. Only synchronous-to-asynchronous edges
  (present and flip waiting on draw) are allowed. A self or future wait (value >= this
  submission's own) stops. So do Scheduler::Wait, WaitQueueSubmitted and WaitDependencies when
  called on the scheduler's own recording thread.
- **Command buffers are bounded.** CommandPool::MaxBuffers = 256 (Apple). At the cap a commit
  waits for the GPU to retire the oldest buffer instead of growing the pool.
- **Recorder-side ticks.** `RecordingTick()` is the tick of the buffer being recorded, set where
  it is begun. StagingBufferPool::Retire's recorded closure uses it instead of the owner's open
  `CurrentTick()`.
- **Chunk budget.** The first chunk is counted (async_submit is set before it is allocated).

Controls added to scheduler-async-submit-test:
- ThreadedRecording toggled off with a submission queued behind a held recorder: Finish
  completes and the fill lands;
- the delayed producer, and the self-wait stop, through WaitBeforeNextSubmit;
- the pool cap, with the GPU held on an external timeline semaphore (at most MaxBuffers command
  buffers, then all fills land when it is signalled);
- a Wait on the recording thread (stop);
- an asynchronous scheduler waiting on another (stop).

## Source review round 3 (Sol-2), as built

- **The pool cap re-reads the GPU tick.** In `ResourcePool::CommitResource`, the search lambda
  captured `gpu_tick` by value, so after waiting for the oldest buffer it still searched with the
  stale tick and waited again forever. It now captures it by reference. Negative control: with
  the by-value capture restored, the pool-cap control reports the hang.
- **Recording-thread identity is a thread_local owner pointer.** `recording_for` is set by
  RecorderThread to its scheduler. StopOnRecorderThread compares it with `this`, with no shared
  `std::thread::id` written by one thread and read by another.
- **The pool-cap control proves the cap.** The pool publishes its size and a count of waits at
  the cap (atomics). The control waits for at least one wait at the cap, asserts exactly
  MaxBuffers command buffers, releases the external GPU gate, then requires every submission to
  complete within 20 s.
- **MoltenVK's own limit is configured above the pool cap.** The first run of the fixed control
  never reached the cap: MoltenVK 1.4.2 allows 64 Metal command buffers in flight per queue by
  default and blocks inside vkQueueSubmit beyond that, on the recording thread, with the static
  submit_mutex held (so every scheduler's submit waits). The held command buffer then ended in a
  Metal GPU timeout (device lost). With MVK_CONFIG_MAX_ACTIVE_METAL_COMMAND_BUFFERS_PER_QUEUE=512
  in the environment, the same binary reached exactly 256 and passed. The Apple instance now sets
  it through VK_EXT_layer_settings (required, as the shelved prefill branch did) to
  CommandPool::MaxActiveMetalCommandBuffers = 4096. A CommandPool stops at construction if the
  live pools' caps plus 256 would exceed it, so the pool cap (256, observed exactly on the device
  in the control) is reached before MoltenVK's limit. Qualifications (Sol-2): the 256 headroom is
  an engineering allowance for Metal command buffers MoltenVK uses outside our pools (presents),
  not a measured or enforced bound. The pool cap waits outside submit_mutex on the asynchronous
  path only (SubmitFromRecorder releases it before Commit); the synchronous SubmitExecution holds
  it through AllocateWorkerCommandBuffers, which can now wait at the cap. Qualified with
  MoltenVK's default prefill (0); the shelved prefill setting does not accompany it. The changed
  capacity changes game behavior only when more than 64 Metal command buffers are in flight, and
  the preview A/B with asynchronous submission qualifies it.
