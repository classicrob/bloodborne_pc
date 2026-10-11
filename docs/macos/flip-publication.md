# VideoOut flip publication audit

Source baseline: macos `756a9b8`, October 11, 2026. This audit distinguishes
command consumption, GPU completion, and display completion. They are different
events; a queue-submit fence establishes only the second.

## Existing order

For a Gnm EOP flip the current path is:

1. `sceVideoOutSubmitEopFlip` registers the one-shot `GfxFlip` interrupt callback.
2. Liverpool parses the patched flip NOP and enqueues `SignalFlip` using
   `Rasterizer::RunInOrder`.
3. On stage B, `SignalFlip` waits for deferred signals and emits the interrupt.
   The callback calls `SubmitFlip(..., is_eop=true)` and `SubmitFlipInternal`.
4. `Presenter::PrepareFrame` records the presentation-image preparation, captures
   the draw scheduler's work semaphore/tick, and flushes that scheduler. It then
   queues the VideoOut request. The frames-ahead limit may wait for an older
   frame; it does not establish completion of this frame.
5. VideoOut's vblank/frame-limit thread calls `Flip`. `RunPresenter` normally
   enqueues a closure for `bb:Present` and returns immediately.
6. **Before that closure need even start**, `Flip` increments `flip_status.count`,
   publishes `flip_arg/current_buffer`, decrements pending counts, triggers the
   flip equeue event, and clears the previous display-buffer label.
7. The swap thread eventually calls `Presenter::Present`. Its GPU submission
   waits on the prepared frame semaphore, blits into the swapchain image, and
   signals the presentation semaphore/fence. `vkQueuePresentKHR` consumes that
   semaphore. Returning from this function is not evidence of display.

With `BB_PRESENT_THREAD=0`, step 7 happens inline before step 6, but completion
of the queue-present call still does not prove the image was displayed.
The current `Frame::present_done` fence protects reuse of the offscreen frame
after its GPU blit; it is not a display-completion fence.

This establishes premature publication relative to GPU execution/display. It
does **not** establish publication before the same frame's stage-A parse: the
EOP request follows that frame's parse and ordered recording. Consequently this
audit does not explain the throttle-off run C's parse-time null SRT table.

The guest gate decoded in `gpu-idle.md` permits reuse when
`flip_arg >= F + 1 - B`. Publishing early can therefore expose frame-slot data
still read by the GPU even though command parsing has finished.

## Installed driver and completion interfaces

The installed x86_64 driver reports MoltenVK **1.4.2** and advertises
`VK_GOOGLE_display_timing`, `VK_KHR_present_id`, and `VK_KHR_present_wait`.
Enumeration is saved locally in `out/flip-audit/vulkaninfo.txt`.

The pinned [MoltenVK 1.4.2 image implementation](https://github.com/KhronosGroup/MoltenVK/blob/v1.4.2/MoltenVK/MoltenVK/GPUObjects/MVKImage.mm)
signals present-wait IDs in the Metal command-buffer completion handler. Its
drawable-presented handler supplies the GOOGLE display-timing history instead.
Therefore enabling present-wait alone would not implement the requested
displayed boundary. The
[swapchain implementation](https://github.com/KhronosGroup/MoltenVK/blob/v1.4.2/MoltenVK/MoltenVK/GPUObjects/MVKSwapchain.mm)
retains at most 60 timing records and consumes them when queried.

The same driver source documents a Metal issue where the last one or two
drawable callbacks may stall. Swapchain replacement/destruction works around
that by changing the layer size. A wait for one callback before allowing the
next presentation would risk both serialization and a progress failure.

## Proposed correction, pending runtime qualification

Renderer reviewed the direction: poll asynchronously; do not block the swap
thread waiting for presentation. Give presentations unique IDs, keep outstanding
guest flip requests in issue order, and poll display timing on the existing
presentation/vblank service path. Publish status, event and previous-buffer
release together only after the corresponding completion is observed.

The implementation must also handle port generations, outstanding requests on
swapchain recreation, skipped/minimized presentations, bounded timing history,
and shutdown. None of these may fabricate a displayed completion. GPU completion
is an explicitly different policy and cannot be substituted silently.

Concretely, the current `Close` resets the single persistent port's counters and
buffer labels. A delayed completion must carry the port generation from submit
and must not decrement a reopened port's pending count or clear its labels.
`Swapchain::Create` currently destroys the old swapchain immediately after a
device-idle wait; that wait does not consume pending display-timing records.
Those records must be resolved or explicitly cancelled without publishing a
display success before destroying the handle. Minimized/acquire-failed paths in
`Presenter::Present` currently skip presenting entirely yet `Flip` still reports
success. Keeping that behavior would invalidate a display-completion claim.

`tools/macos/present_timing/run.sh` is a manual installed-driver qualification.
Run it through the quick pool with the GPU exclusive lock. It submits 30 FIFO
presents, polls their timing IDs, checks each report against the GPU completed
timeline (the first GPU submit is held behind a 100 ms host-signaled timeline),
compares zero-timeout present-wait, and polls for two seconds after
the last frame. Exit 2 means some final display callbacks remain missing; it is
not a passing completion qualification. Exit 3 means the held-gate negative
control was unobserved, for example because submit/present blocked until release.
The log records both calls' begin/end times and gate-release time, and only
counts the negative control while the host has not begun signaling the gate.
The timeline check proves completion by observation time, not the internal
ordering of the drawable callback: the latter inference additionally uses the
pinned driver source. This fixture does not yet prove
minimized/recreated swapchain behavior or exercise the production VideoOut path.

## Runtime result and implementation decision

The signed x86_64 qualifier exited **2** on October 11, 2026, under the quick
pool and exclusive GPU lock. Installed MoltenVK dylib SHA-256:
`583db6b034852823ad40c0c80fe0f5f145bd55b031310d356c29d685efe8d61c`.
Compiler: Apple clang 21.0.0 (`clang-2100.0.123.102`); expanded build flags and
output are in `out/flip-audit/present-timing.log`.

- GOOGLE timing reported exactly IDs 1 through 28 of 30 FIFO presents. IDs 29
  and 30 were still absent after the two-second tail with no further presents.
- The observed GPU timeline reached 30, and zero-timeout present-wait for ID 30
  returned success. That did not imply a corresponding drawable timing record.
- The held-gate negative control was observed: one poll saw an unsignaled host
  gate, GPU tick zero and no presentation timing row. It was not an unobserved
  control hidden by a blocking submit/present call.

This reproduces the documented final-callback progress limitation. It does not
prove the two frames were not displayed, nor that callbacks can never arrive;
it fails qualification for a guest that stops submitting while awaiting its
last flip. No production publication change was made by this audit unit.

After this result, the orchestrator explicitly selected **GPU retirement of the
presenter's work for the frame** as the emulator's publication boundary. This
supersedes the earlier physical-display requirement. The rationale is resource
lifetime: after the presenter's final read/copy of the guest framebuffer
retires, the swapchain owns the pixels independently and the guest can reuse
its framebuffer. Vblank continues to model pacing. This is an explicit policy
decision, not a claim that Metal command completion is physical display.

Opus-PM4 owns the Apple-only implementation through ordered GPU completion,
with an A/B toggle; Opus-Renderer reviews and must confirm there is no later
guest-framebuffer read or zero-copy presentation path. Status/count, flip event
and previous-buffer release must move together. The port-generation and
swapchain-lifetime considerations above still apply; a skipped presentation
must at least retire every preceding GPU read before releasing guest storage.
