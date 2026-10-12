# Render pass trace (diagnostic, off by default)

The render pass census ("Render passes, last N frame ends" in the parse-split block) counts the
passes begun per frame and what ended each. The trace records each pass of a few frames, so the
attachment traffic and the fate of every store can be worked out offline. On a tile GPU each pass
loads its attachments into tile memory (LOAD) and writes them back (STORE). The scheduler
(`Scheduler::BeginRendering`) loads every attachment the guest does not clear and stores every
attachment. It never uses DONT_CARE or NONE.

## Settings

- `BB_PASS_TRACE=<first>[+<count>]`: traces the frames from frame end `<first>` (counted from 1,
  as the census counts them) for `<count>` frames (1..64, default 1).
- `BB_PASS_TRACE_OUT=<path>`: the JSON-lines file, written at each traced frame end. It is
  required when `BB_PASS_TRACE` is set.
- A malformed value stops.

With the perf replay, each iteration is one frame end. For example, `BB_PASS_TRACE=10+3` traces
iterations 11 to 13. "Pass trace: frames ... written to ..." marks the end.

## Records

- `begin`: the pass extent and layers, and `resumed` (exactly the state of the pass that ended
  just before it). `same_targets` means the same views and extent, possibly with other layouts or
  clears. Each attachment aspect lists slot (`c0`..`c7`, `depth`, `stencil`), image uid,
  `known`, VkFormat, texel bytes, the level's extent, base layer, layers, level, samples, layout
  and load (`load` or `clear`).
- `end`: the census's cause (`target change`, `submit`, `barrier`, `other`), the attributed ender
  as `lib+0x...` (the FlushBarriers caller for barriers, else the EndRendering caller), and the
  rasterizer draws in the pass.
- `end` with `partial`: the pass was open when the trace started. Its attachments and earlier
  draws are unknown, and the report does not count it.
- `use`: every access requested through `Runtime::Transit`. It lists uid, layout,
  VkAccessFlags2, VkPipelineStageFlags2, whether it covers the whole image, and
  `requested_in_pass`. Requests are recorded before the barrier flush that may end the open pass,
  so `requested_in_pass` does not prove the access ran inside that pass.
- `bind`: an image bound to a draw or dispatch (sampled, storage or target), once per image per
  draw, after it was recorded (`Rasterizer::ResetBindings`). These cover the reads that need no
  layout change and so pass no `Runtime::Transit`.
- `frame`: the passes, draws, accesses and binds of the frame.

Image uses recorded straight into command buffers, with neither `Runtime::Transit` nor a binding,
are not seen.

Attachments are resolved only for the next pass. The rasterizer provides its full render state's
targets just before the pass begins. A pass begun from a render-state memo hit (after a barrier
ended the open pass) gets the snapshot kept with that memo. The list is cleared at every begin, so
a view handle noted earlier and reused cannot inherit stale metadata.
- `known`: the pass uses the guest image's own view.
- A redirect or scene proxy (other storage, other extent) keeps the guest uid with `known` 0.
- The blit helper's views, the object motion attachment, and passes begun from a cached state
  without a memo hit are unresolved (uid 0).

The report counts unknown attachments but gives them no bytes. Only the rasterizer's draws are
counted.

## Report

```
python3 tools/macos/pass_trace.py trace.jsonl --lib <the run's libbbgpu.dylib> --gpu-busy-ms 34
```

The report gives, per frame:
- passes by how they began, with resumes by what ended the pass before;
- passes by what ended them, symbolicated with atos;
- passes by draws (with or without a clear);
- attachment coverage: known/all attachment aspects by begin kind (aspects, not bytes);
- bytes loaded and stored by aspect, for known aspects only;
- each store's fate.

All bytes are logical: the render area times the layers, samples and aspect bytes. Apple GPUs
compress render targets losslessly, so the bytes and levers are upper bounds on traffic, not
measured bandwidth. Depth and stencil are separate aspects, as Metal stores them. D24S8 and D16S8
have no Metal format; MoltenVK uses D32Float_S8 (4 + 1 bytes).

Fates are decided per image uid and aspect (color, depth, stencil), by the next event touching
them:
- loaded by a later pass, or loaded by a resume;
- read: a requested read (`use`), or the image bound to a draw or dispatch (`bind`);
- dead candidate: a later pass clears the same aspect at the same level, covering its layers and
  area; or a requested whole-image clear;
- written first: a clear of another level or a smaller area, or another write-only access;
- used through a redirect or proxy (the guest uid with `known` 0);
- read-only candidate: the pass's layout could not change the aspect;
- unused until the trace ends: possibly read next frame, not counted as dead.

A pass with unresolved targets (uid 0) may have read any pending store. Stores pending across one
are never called dead ("cleared after an unresolved pass"). Dead and read-only remain candidates:
a use with neither `Runtime::Transit` nor a binding would not be seen. Confirm a lever's images
before acting on it.

The model compares logical traffic over a bandwidth, plus a cost per pass, with the measured GPU
busy. The levers are upper bounds, listed separately and not summed:
- merging resumes, by the class of the ender between them. A resume is excluded when an access or
  bind between the two passes touched one of its attachments.
  - barrier enders: a buffer or image barrier cannot go inside a dynamic-rendering pass, so the
    barrier's producer would have to move out of the pass;
  - transfer enders: what pass hoisting moves;
  - dispatch, HLE and other enders.
  - Without `--lib`, other enders are unclassified.
  - A resume with unresolved targets adds no bytes, and the report says how many there were.
- not storing read-only candidates (STORE_OP_NONE);
- not storing dead candidates (DONT_CARE);
- dropping empty passes (no draw, no clear).

Test: `tests/test_pass_trace.sh`, run by `build.sh --test`. It checks a known sequence through
the header and the report's numbers, a hand-built trace for each fate rule and the merge classes,
and that malformed settings stop.
