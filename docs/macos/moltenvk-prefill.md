# MoltenVK Metal command buffer prefill (Apple): shelved

## The idea

MoltenVK 1.4.2 defaults to `MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS=0`. It keeps every recorded
vkCmd* and encodes the whole command buffer into Metal inside `vkQueueSubmit`. `Scheduler::Flush`
calls that on the draw recording thread (stage B), 48-80 times a frame. In the PREVIEW-20 quiet R0
parse split, stage B spent 27-48 ms per frame in "queue submit", while the Vulkan recording
thread was mostly idle. Prefill style 2 encodes each command into Metal when it is recorded,
on the recording thread. It is set through `VK_EXT_layer_settings` (layer "MoltenVK"), or the
environment variable when no layer setting is passed. The source (MVKCommandBuffer.mm) shows the
prefilled Metal command buffer is committed at submit, so submission order would be unchanged.

## The A/B (Playtest, PREVIEW-20, quiet R0, env var 2 vs unset)

- **Encoding moved:** stage B "queue submit" fell from 23-41 to 2.6-4.2 ms per frame. The
  recording thread now encoded draws (6-12 ms), rendering begin/end (8-14 ms), copies and
  dispatches.
- **Stage B now waited for it instead:** "recorder sync" rose from 7-12 to 25-44 ms, because
  every Flush syncs the recorder. The frame time was unchanged (about 88 ms; 11.3 FPS idle).
- **Wrong frames:** 6 of 11 captures were a full-screen beige/white frame with only the HUD,
  against 0 of 11 without prefill. No MoltenVK error was logged.

So prefill is shelved. Metal encoding is 35-45 ms per frame of serial work wherever it runs, and
stage B synchronises with it at every Flush. The levers are fewer render passes, fewer or
asynchronous submissions, and parallel encoding.

## What stays: Scheduler::Close for teardown

`Presenter::~Presenter` used to call `Finish` (which opens a new command buffer, with BB_PERF
timestamps) and then reset that buffer unsubmitted. MoltenVK commits such a buffer when prefill
is on (`clearPrefilledMTLCommandBuffer`), outside the scheduler's ticks, while resources are
being destroyed. `Scheduler::Close` (Apple) does the teardown without opening a new buffer:
- it submits the open command buffer through the normal submission (the submit callback may
  still record, e.g. the rasterizer's pending barriers), opens none, seals recording, and waits
  for that submission's tick;
- recording afterwards stops; Flush and Finish return at once;
- `~Scheduler` closes a scheduler whose open buffer has no recorded command, and stops for one
  that has;
- it waits for the GPU only, not priority operations or deferred signals;
- `Presenter::~Presenter` closes its three schedulers.

scheduler-close-test covers it (BB_PERF on):
- an unsubmitted fill, with the threaded recorder and on the caller's thread;
- a submit callback recording into the final submission;
- the two stops (exit 23 with their message) in forked children.
