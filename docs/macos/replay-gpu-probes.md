# Destructive replay GPU attribution probes

These experiments change rendered contents, shader side effects and later dependencies.
They are invalid for correctness, image quality, acceptance or shipping configuration.
Their timing deltas are diagnostic estimates, not rigorous savings bounds, and cannot be
added together. No optimization is justified by the probe alone.

## Evidence before implementation

The protected capture `frame-20261012-123949-77481` has 73 submissions and 357 saved
GPU-modified images. Producer source is `c7bd67266544125e19fea8775beb0c7c32f0dcee`,
library SHA256 `7e1e61c68c15f355cc16d957b158d35e52ec87beb81b2a5ee8421a660238849d`.
It follows the quiesced-restore fix. Legacy host reset identity still needs the replay
owner's explicit overlay migration and loader/cross-build qualification before execution.
The original capture and route provenance must remain intact.

Bounded image-header inspection found 541,833,986 bytes of saved image payload. Of those
images, 127 have prior attachment usage, with 459,935,594 bytes of mip-zero payload.
The stock includes nine 1920x1080 RGBA16F images (149,299,200 bytes), ten 1920x1080
RGBA8 images (82,944,000 bytes, seven with attachment usage), four 1920x1080 D32S8
images (41,472,000 bytes) and a 4096x4096 D32 image (67,108,864 bytes).
These are backing inventories, not a list of attachments used in every frame.

Historical clinic census counted 263.2 passes/frame and 41.2 same-state resumes. A
*hypothetical* full-1080p RGBA8+D32S8 pass loading and storing both aspects moves
37,324,800 logical bytes: 9.824 GB for 263.2 such passes, or 1.538 GB for 41.2 pairs.
This illustrates the scale worth measuring; it is neither observed traffic nor an upper
bound. Mixed extents, clears, coverage, aspect use, compression and intermediate readers
prevent applying this multiplication as a bandwidth or time claim. Fresh pass trace must
establish these facts. Earlier per-split timing was invalidated by Low Power Mode.

The post-restore-fix `out/replay-ab/ab-selftest2/run-1-A.log` at `df541b1` still reports
57.8 MiB of image uploads per steady iteration. Its old GPU-busy-zero output is not timing
evidence. Producer route `20261012-123649-1080p-PERFCAP/game.log:5015` records 1,100
refreshes / 4,530 MiB entered **and copied** over ten seconds, including six 1920x1080
targets dirtied by SDK copy/fill shaders. This supports real buffer-to-image/detile work;
it does not establish unnecessary CPU reuploads or new allocations. The legacy byte
counter increments before per-mip hash skips, so it alone is insufficient. The older
117 MB/live comparison remains unqualified. Never skip these uploads: a previous skipped
SDK copy blackened the scene. Any image-to-image recognition needs exact semantic proof.

## Controls and ownership

`BB_REPLAY_GPU_PROBE` is latched on first use:

| Value | Changed commands |
|---|---|
| unset, empty, `off` | None |
| `scissor-1px` | Clamp each nonempty guest scissor to at most 1x1, preserving its offset. Empty scissors stay empty. Viewports, vertex/geometry work and compute are unchanged. |
| `discard-load-store` | Change bound attachment LOAD to DONT_CARE and STORE to DONT_CARE. Explicit CLEAR is preserved. Color, depth and stencil are counted independently. |

Invalid settings abort. Both enabled modes require `BbReplay::MeasurementActive()` at
every relevant pass/draw path before changing commands. `BB_REPLAY_RUN` or any other
environment claim cannot authorize them. The replay driver owns that read-only API
(`de76286d`); it starts false and becomes true only after validated setup and each
iteration's untimed restore. The helper does not change replay lifecycle, barriers,
submissions, retirement or restoration. Renderer owns the helper, rasterizer and scheduler
sites; Replay owns integration in its iteration loop. Shaders reviews the combined change.

Replay integration must save `ReadCounters()` after entering the measurement scope,
before `t0`, and call `ReportDelta(iteration, before)` after its existing GPU completion
wait and timing sample, but before leaving that scope. Require `BB_REPLAY_GPU_WAIT=1`
for these experiments; the helper refuses otherwise and also refuses `BB_WORK_RETIRED`
values beginning with `0`, matching Liverpool's recording-retirement bypass. The report has rasterizer draw sites, clamped scissor rectangles,
discarded passes, store aspects and load aspects, and an invalid-for-quality marker.
Counters describe issued paths, not measured fragments or physical memory transactions.
The original pass trace records guest attachment state; its logical load/store estimate
must not be mistaken for the mutated commands in the discard arm.

## Minimal run plan after qualification

1. Qualify the reused capture and its reset overlay. Pin source, dylib, config and capture
   hashes. Confirm actual internal extents in the trace. A 960x540 historical output does
   not prohibit attributing validated 1920x1080 internal work, but does not meet native
   output acceptance. Use one baseline build, with the probes off.
2. Take one short baseline attribution run with `BB_PERF=1`, pass trace and ring counters,
   then a separate timing-only control. Preserve frame IDs, aspect coverage, end reasons,
   draw/dispatch/submission counts and refresh entered/copied/hash-skipped counts. No
   per-frame stock multiplication substitutes for the trace.
3. With the same executable/harness, run baseline, scissor, baseline, discard, baseline
   as separate fresh processes from the same capture/overlay. Use one factor per arm,
   identical iteration count and `BB_REPLAY_GPU_WAIT=1`. Use the heavy wrapper's exclusive
   GPU/CPU-quiet slot and settle, reserving bounded logs and no new capture.
4. Start with a few iterations, retaining every individual result. Check nonzero probe
   counters, equal expected command counts and normal stop behavior. A changed wait,
   command count, fault or nontermination disqualifies timing comparison. The first
   iteration includes compilation/warm-up; do not call it steady state. Extend only if
   the short run is informative and repeatable.
5. Restored command-processor ranges do **not** restore all shader-written images and
   buffers between iterations. Discard may corrupt later iterations; scissor may remove
   shader side effects. Even within the first iteration, later passes observe changed
   inputs. Label all such results as workload sensitivity, not the cost of an isolated
   load/store or fragment stage. A warm, exact common-input comparison needs additional
   restoration support and is outside this prototype.

A large scissor response prioritizes measuring expensive raster/fragment work and its
dependencies. A large discard response prioritizes tracing safe load/store elimination
and producer ordering. A weak response does not rule out either cost because the arms
change other work. Before implementing a production optimization, derive its maximum
possible gain from the exact affected operations and valid baseline cost, and obtain an
independent correctness review.

## Current validation

Standalone `tests/test_pass_trace.sh` passed through the shared heavy wrapper with Xcode
27.1, x86_64, C++20, `-Wall -Wextra -Werror`. It covers off behavior, both transformations,
empty/1x1 scissors, clear/null attachments, enabled counters, malformed setting refusal,
and refusal when only an environment flag claims replay. The follow-up run also passed
completion-wait and recording-retirement bypass negative controls. Existing pass-trace analyzer
fixtures also pass. This does not compile Vulkan renderer translation units, validate
the production runtime guard, or execute a GPU probe. Full integration and review remain.
