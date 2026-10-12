# Parse split: where the GPU-side threads' time goes (macOS, always on)

R0 (docs/macos/throttle-relaxation.md) showed that the whole sceGnmSubmitDone-to-GpuIdle wait is
"parse and queue". The existing stats show that the GPU command thread (stage A) is on the CPU
for only 9-23 ms of a 100-200 ms frame. It spends most of the rest waiting for the draw recording
thread (stage B). These counters split each of the three threads' wall time.

## How it counts (gpu/shim/bbport_parse_split.h)

Each thread's time is partitioned exactly. Every transition first charges the time since the
previous transition to its owner, then changes state. Transitions are a mark, or a scope's start
or end.
- `BbSplit::Mark(bucket)` opens a bucket. It does nothing when that bucket is already open.
- `BbSplit::Scope{kind}` (RAII) is a wait or a named piece of work. While it is open it owns the
  thread's time, and inner scopes take their own. A mark inside a scope only changes the bucket its
  time is listed under (per-bucket scope time, e.g. deferred-signal waits by stage A packet class).
- `BbSplit::Section{account, bucket}` opens a bucket for its lifetime on that account's thread
  only, then reopens the previous one. This is for code that both stages run.
- Coroutines: liverpool.cpp's YIELD and RESUME save the open bucket and reopen it afterwards
  (`Suspend`/`Resume`). A packet that yields gets its class back when it resumes, and a parent
  gets its bucket back after a CE or nested IB child it resumed returns. The time a coroutine is
  suspended stays with whatever ran meanwhile. A scope open across a yield stops, as does a scope
  still open when a queue task returns to the scheduler loop. No scope spans a yield today.
- A drain of an idle draw pipe is not timed: it returns at once.
- No scope may span a `siglongjmp` (e.g. a speculative read's fault recovery). It would leave the
  scope open.
- Counts use `__rdtsc`, with no locked instructions. Each thread is the only writer of its
  account, and the reporter reads it with relaxed loads.
- A thread without an account records nothing. For example, the texture helper and the
  preparation workers are not counted.
- The macros `BB_SPLIT_SCOPE`, `BB_SPLIT_SECTION` and `BB_SPLIT_MARK_ON` compile to nothing off
  Apple.

**Overhead** (`tests/bench_parse_split.sh`, process CPU time, x86_64 under Rosetta on the M4 Max,
accounting on vs off). The benchmark is synthetic and single-threaded, runs off then on in a fixed
order, and its extra scopes repeat one kind. Read the numbers as an illustration of scale, not a
measured bound for gameplay or for the composed hooks. The small stage A figure is especially
noise-sensitive. Alternate the order and report the spread before using them for a performance
claim.
- one `__rdtsc` costs about 11 ns under Rosetta;
- stage A: 0.6-1.4 ns per packet (same-bucket marks are free), about 6-11 µs per frame at 800
  draws;
- stage B with these marks: about 135 ns per draw (11 transitions), so 0.11 ms per frame at 800
  draws;
- every further scope per draw adds about 22 ns. Fifteen more add about 320 ns per draw, or 0.26
  ms per frame at 800 draws.

## Accounts and buckets

**Stage A, the GPU command thread** (liverpool.cpp):
- one bucket per graphics packet class: registers, draws, indirect draws, dispatches, labels,
  WAIT_REG_MEM, memory packets, INDIRECT_BUFFER, other;
- constant engine and compute queue packets;
- host commands, the scheduler loop, frame end and GpuIdle;
- waiting for submissions.

**Stage B, the draw recording thread** (vk_draw_pipe.h, vk_rasterizer.cpp):
- no packet, decode, in-order tasks, special draws, dispatches, indirect draws;
- for direct draws: setup, render state, bind, begin rendering, record;
- bind buffers and bind textures, wherever they run on stage B;
- after packet.

**Vulkan recording thread** (vk_scheduler.cpp): idle, executing chunks.

**Scopes:**
- stage B drains, by reason: DUMP_CONST_RAM, idle, submit/flush, other packets;
- draw pipe full;
- deferred signals (`WaitDeferredSignals`, labels at GPU completion);
- recorder signals, recorder sync, host copies, GPU ticks, presentation label;
- queue submit.

Opus-Renderer's scope kinds, the recording internals (inside whichever bucket is open). Per-call
lookups (image and buffer lookups, samplers) have no scope: they are what remains of "bind
textures" / "bind buffers" / "render state". At about 22 ns per scope under Rosetta, stage B pays
two per draw (descriptor build, dynamic state) plus the rare ones:
- image refresh (RefreshImage: detile and upload records), texel sync (SynchronizeMemoryFromImage:
  an image tiled into its guest memory for a texel-buffer read);
- pipeline build wait (WaitBuiltSlow only), descriptor build (Pipeline::BindResources), dynamic
  state (DynamicState::Commit), barrier flush (Runtime::FlushBarriers, when it has barriers), render
  pass switch (Scheduler::BeginRendering when the pass changes, EndRendering);
- on the Vulkan recording thread, inside "executing chunks": MoltenVK's vkCmd* by type: pipeline
  bind, descriptors (push constants, push descriptors, set binds), draws (with vertex input,
  vertex and index buffer binds), dispatches, rendering begin/end, barriers, copies/fills/clears
  (about four per draw there). The rest of a chunk (dynamic state setters, other commands) stays in
  "executing chunks". MoltenVK records Metal work in these calls or at queue submit (in "queue
  submit"); these scopes do not see the Metal driver after submission.

## Flush census (the draw scheduler's submissions)

With labels at GPU completion, a label is written once a Vulkan submission containing its work
completes. That makes labels the main driver of submissions. Each submission also ends any open
render pass and syncs the Vulkan recording thread.

Every draw-scheduler submission is counted by reason, from a thread-local reason the caller sets:
- frame end;
- unbatched labels (one per label: stage A, or recording not deferred);
- stage B's label batch, at an idle packet boundary or at LabelBatchAge (500 us);
- Finish;
- other.

The census also counts how many submissions found a render pass open, and the age of the oldest
label in each batch at submit. It also gives labels written per frame and their issue-to-write age
(mean and window maximum): the delay a guest waiting on a label sees. Asynchronous submission and
later label flushing must keep this within about one submission's encode time. Compare both the
mean and the maximum across arms over the same windows, and read the maximum over several windows,
since one window's maximum is a single extreme.

It counts the WAIT_REG_MEMs stage A met early (their in-order markers), and the in-order
WRITE_DATAs behind one (the write barrier, gpu-idle.md):
- how many submitted stage B's label batch before waiting. These submissions are counted under
  "label batch at idle";
- of those, how many batches held only labels after the ones the WRITE_DATA needs. Such a
  submission does not order that WRITE_DATA; it only writes the later labels sooner;
- how many then waited for labels, and the mean wait.

One line is printed in the parse-split block. Renderer's render pass census ("Render passes ...")
attributes each pass end. A pass counts as ended by a submission when it is still open after the
submission's barrier flush, which can close passes itself (counted there as "barrier"). So "ended
an open render pass" here equals that census's "submit" enders.

## Constant ring (stage A's copies of small constants)

Stage A copies small read-only buffer resources into the constant ring at decode
(`Rasterizer::CollectRingBindings`), so stage B binds them without `ObtainBuffer`. A resource is
skipped, and bound through `ObtainBuffer` at stage B, when:
- it is larger than the stream threshold;
- it is GPU-modified;
- an unreached pending GPU-write note overlaps it.

Notes come from `WRITE_DATA`, DMA and written V#s (`NotePendingGpuWrite`). Overlapping notes
merge, and each new one moves the entry's position on. The "Constant ring per frame" line in the
block gives:
- the copies (and KiB), and copies that did not fit (ring full);
- the skips by reason, with the pending-write skips behind a note of 64 MiB or more counted apart;
- the notes per frame, with those of 64 MiB or more and how many of them were written V#s.

A written V# of a whole arena (the clinic replay notes ~2.18 GB ones) covers every constant in its
range while unreached. If the "behind a note >= 64 MiB" count is a real share of the copies,
clamping such notes to the range the shader can write is a cheap lever.

## Output

One block every 600 frame ends, printed from stage A:

```
Parse split, last 600 frame ends: <ms> ms per frame, <n> draws per frame (stage A draw bucket <us> us wall per draw); ms per frame by thread (accounted / wall)
  stage A (GPU command thread, <accounted %>, on CPU <ms> per frame): <bucket ms, ...>; in scopes: <kind ms, ...>
    deferred-signal waits during: <stage A bucket ms, ...>
    stage B drains at other packets during: <stage A bucket ms, ...>
  stage B (draw recording thread, <%>, on CPU <ms> per frame): ...
  Vulkan recording thread (<%>, on CPU <ms> per frame): ...
```

"stage A draw bucket us wall per draw" is the wall time of stage A's direct-draw bucket
(exclusive of named scopes) over the window's counted direct draws, so arms with different draw
counts compare. It is not CPU time and not a draw's whole processing cost.

"on CPU" is the WHOLE thread's user + system CPU time (thread_info) per frame over the window, not
any one bucket's. Compare it with the thread's accounted wall time: well below it means the
thread was blocked (a lock, a wait) or not scheduled for part of the window. About equal to it
with slower draws does not by itself establish a slower core or cache misses: spinning, extra
work or a different work mix fit too; attribute with scopes or a profiler.

"n/a": the thread has not attached (for example no recording thread with threaded recording off,
which is expected), thread_info failed, or the first window (the baseline is primed, nothing
reported). The Mach port is the attached thread's while it lives; an account has no detach, so
a replaced thread would need a fresh baseline (one lifetime per run today). Numeric samples are
required only for threads that are running, after the baseline window.

How to read it:
- Stage A charges its open interval before each report, so its window is effectively closed (the
  counters, the TSC and the wall clock are sampled one after another). The other
  threads' counters are sampled as they stand, so their windows are approximate at both edges.
  An interval open at a report is charged to the next window, which therefore has carry-in as
  well as carry-out. Their accounted percentage is close to 100%, not exactly 100%.
- Waits overlap across threads. For example, stage A waits in a drain while stage B waits on the
  recorder. Read each thread's line on its own.

## Lock waits (gpu/shim/bbport_lock_wait.h)

Scopes named "lock wait: <lock>" are contended acquisitions of our own locks, charged to the
thread that waited:
- the texture cache mutex and its samplers mutex;
- the buffer cache's region locks and the page manager's range locks;
- the staging pool mutex and the draw preparation mutex;
- the pipeline programs lock (shared or exclusive).

A lock tries first. Only a failed try opens the scope, for the time lock() / lock_shared()
then takes: spinning and scheduling delay included, not necessarily sleeping. The hold is not
measured.

What it does not cover (read a zero line accordingly: it does not rule out all contention):
- try_lock() / try_lock_shared() themselves are not timed. So the retries of Common::LockRange /
  TryLockRange (the page manager's range locks) leave part of their acquisition outside the
  scope.
- Only threads attached to the parse split (stage A, stage B, the recording thread) contribute.
- The scheduler's mutexes are not wrapped: their waits show partly as "recorder sync" and the
  related scopes, not as every acquisition.
- MoltenVK's internal locks are not visible here.

A controlled held-lock test would pin the attribution if this diagnostic is extended.
