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
  clears. Each attachment lists slot (`c0`..`c7`, `depth`, `stencil`), image uid, VkFormat, texel
  bytes, image extent, level, layout and load (`load` or `clear`).
- `end`: the census's cause (`target change`, `submit`, `barrier`, `other`), the attributed ender
  as `lib+0x...` (the FlushBarriers caller for barriers, else the EndRendering caller), and the
  rasterizer draws in the pass.
- `use`: every access requested through `Runtime::Transit`. It lists uid, layout,
  VkAccessFlags2, VkPipelineStageFlags2, whether it covers the whole image, and
  `requested_in_pass`. Requests are recorded before the barrier flush that may end the open pass,
  so `requested_in_pass` does not prove the access ran inside that pass. Image uses recorded
  straight into command buffers without `Runtime::Transit` are not seen.
- `end` with `partial`: the pass was open when the trace started. Its attachments and earlier
  draws are unknown, and the report does not count it.
- `frame`: the passes, draws and accesses of the frame.

Attachments are resolved only for the next pass. The rasterizer notes its full render state's
targets just before the pass begins, and the list is cleared at every begin, so a view handle
noted earlier and reused cannot inherit stale metadata. A target is resolved only if the pass uses
the guest image's own view. Upscaler redirects, scene proxies (other storage, other extent), the
blit helper's MSAA copies and the object motion attachment are unknown. The report counts them
but gives them no bytes. Only the rasterizer's draws are counted.

## Report

```
python3 tools/macos/pass_trace.py trace.jsonl --lib <the run's libbbgpu.dylib> --gpu-busy-ms 34
```

The report gives, per frame:
- passes by how they began, with resumes by what ended the pass before;
- passes by what ended them, symbolicated with atos;
- passes by draws (with or without a clear);
- bytes loaded and stored by aspect;
- each store's fate.

The fates are:
- read later (sampled, copied, or loaded by a later pass);
- loaded by a resume;
- dead candidate (the next recorded use clears it, as a pass's clear load or a whole-image
  clear);
- read-only candidate (the pass's layout could not change it);
- written first, partially or with unknown extent;
- unused until the trace ends (possibly read next frame: not counted as dead).

Dead and read-only are candidates, not proofs. A use recorded without `Runtime::Transit` between
the store and the clear would not be seen. Before acting on a lever, confirm its images.

The model compares traffic over a bandwidth, plus a cost per pass, with the measured GPU busy. It
lists four levers, each an upper bound:
- merging resumes (the resumed pass's loads plus the previous pass's stores, if the op between
  does not touch the attachments);
- not storing read-only candidates (STORE_OP_NONE);
- not storing dead candidates (DONT_CARE);
- dropping empty passes.

Depth/stencil bytes are per aspect (Metal keeps them apart; D24S8 is D32S8 on Apple GPUs).
Accesses are tracked per image, not per level or layer.

Test: `tests/test_pass_trace.sh`, run by `build.sh --test`. It checks a known sequence and the
report's numbers, and that malformed settings stop.
