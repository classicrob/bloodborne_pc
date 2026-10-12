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
label in each batch at submit. It also gives labels written per frame and their issue-to-write age (mean and
window maximum): the delay a guest waiting on a label sees. Asynchronous submission and later
label flushing must keep this within about one submission's encode time. One line is printed in the parse-split block. Renderer's render
pass census ("Render passes ...") attributes each pass end. A pass counts as ended by a submission
when it is still open after the submission's barrier flush, which can close passes itself
(counted there as "barrier"). So "ended an open render pass" here equals that census's "submit"
enders.

## Output

One block every 600 frame ends, printed from stage A:

```
Parse split, last 600 frame ends: <ms> ms per frame, <n> draws per frame; ms per frame by thread (accounted / wall)
  stage A (GPU command thread, <accounted %>): <bucket ms, ...>; in scopes: <kind ms, ...>
    deferred-signal waits during: <stage A bucket ms, ...>
    stage B drains at other packets during: <stage A bucket ms, ...>
  stage B (draw recording thread, <%>): ...
  Vulkan recording thread (<%>): ...
```

How to read it:
- Stage A charges its open interval before each report, so its window is effectively closed (the
  counters, the TSC and the wall clock are sampled one after another). The other
  threads' counters are sampled as they stand, so their windows are approximate at both edges.
  An interval open at a report is charged to the next window, which therefore has carry-in as
  well as carry-out. Their accounted percentage is close to 100%, not exactly 100%.
- Waits overlap across threads. For example, stage A waits in a drain while stage B waits on the
  recorder. Read each thread's line on its own.
