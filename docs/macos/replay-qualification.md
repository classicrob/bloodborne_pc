# Replay qualification and existing-build A/B

`replay-ab.sh A_MANIFEST B_MANIFEST --capture DIR --iters 30 --order ABBA`
uses existing artifacts only. Build manifests record a full source SHA and absolute paths plus
SHA256 for `artifacts.bb-probe` and `artifacts.libbbgpu.dylib`; the latter must be the exact
`gpu/libbbgpu.dylib` beside the executable. Shared previews require `qualification.test_gate=PASS`.
`--qualification-only` permits missing/NOT_RUN/SKIPPED gates for correctness diagnosis and marks
results `SOURCE GATE UNQUALIFIED`; FAILED gates are refused. Explicit
`--qualification-only` keeps that verdict even when both manifests say PASS.
No replay proves live FPS or pixel correctness.

The runner hashes inputs outside timing in a build slot, then launches each run under heavy.sh
with an exclusive GPU, stop-mode CPU quiet and settle (default load below 6, bound 120 seconds).
Each run records host power/thermal/memory/load after settling. A fresh output directory retains
config/input/binary hashes, plans, heavy output, BB_PERF and per-run results. Environment overrides
are parsed as arguments, never shell code; capture/iteration/timestamp/lock settings are protected.
Only unchanged pinned binaries and boot inputs are used. STOP/watchdog, nonzero exit,
missing/duplicate iteration, missing image round trip or missing/nonpositive
submit-to-GPU-done wall samples fail orchestration. Wall and CPU summaries cover
iterations 2..N. The wall summary selects the explicit `submit-to-GPU-done`
field, never the earlier `submit-to-idle` field on the same line. A standalone
non-strict summary may show idle-only historical data, explicitly unqualified.
`replay_summary.py --expected N --json LOG` validates complete wall samples.

GPU timestamps can remain VK_NOT_READY after completion and are collected at a
later submission. Totals deltas have no exact iteration membership. New
`GPU timestamp collection window at iteration N (S submissions): X ms` lines
and old `GPU busy of iteration N` lines are therefore collection-window
diagnostics only. Their median is named `gpu_collection_window_median_ms`;
the compatibility `gpu_busy_median_ms` stays null and is never accepted.
Missing diagnostic timing is MISSING, not zero, and does not invalidate a
complete GPU-done wall measurement. The legacy `--require-busy` option only
requires a positive diagnostic collection sample; it cannot qualify per-iteration
busy. Aggregate busy_union also needs a closed, complete reporter interval;
this runner does not infer one from an iteration number.

## Result verdicts

Per-run result JSON, per-run/final console output and a qualification record
appended to each successful raw log carry the verdict. Copying a log and running
`replay_summary.py` retains its source gates, qualification-only flag, diagnostic
reasons and cross-run comparison status. A raw historical log without that
record is `SOURCE GATE UNQUALIFIED` and `COMPARISON NOT EVALUATED`. The record
persists `expected_iterations` and the matched workload signature; detached
summaries revalidate exact iteration coverage, steady-state completion, image
round-trip proof and positive GPU-done wall samples against that count. Missing
iteration-count provenance cannot qualify a run. The runner refuses any game
output containing a preexisting qualification record before appending its own.

- `QUALIFIED RELATIVE ATTRIBUTION`: both source gates PASS, qualification-only
  is false, no diagnostic mode/perturbed schema, complete positive GPU-done wall
  samples and identical steady draw/dispatch/guest-submission counts within and
  across every arm/run. Only submit-to-GPU-done wall is an accepted metric;
  collection-window diagnostics, pixels and live FPS stay unqualified.
- `SOURCE GATE UNQUALIFIED`: either source gate is absent/NOT_RUN/SKIPPED, or
  `--qualification-only` was explicitly requested. Exit 0 means the requested
  correctness-diagnostic orchestration completed, not accepted timing.
- `DIAGNOSTIC PERTURBED`: explicit `--diagnostic`, phase instrumentation, or
  pass/ring/refresh trace overrides. Such overrides require `--diagnostic`
  before launch; an unexpectedly perturbed BB_PERF schema also marks every
  result and exits nonzero. Timing-only ablations must use `--diagnostic` even
  if no traced variable is involved. They never qualify correctness/acceptance
  and need a separate unperturbed control.
- `INCOMPARABLE`: steady draw/dispatch/guest-submission counts differ within or
  between runs. Retain per-run and combined result JSON and diagnostic timings,
  print this verdict and exit 2, including intentional diagnostic ablations.
  Independent correctness proof is required before interpreting changed work.

Any enabled `BB_REPLAY_GPU_PROBE` requires `--diagnostic` and makes results
incomparable for normal timing, even if draw counts match. Empty/0/off probe
values are inactive. A raw `INVALID_FOR_CORRECTNESS_QUALITY=1` marker independently
forces `DIAGNOSTIC PERTURBED`, `INVALID FOR CORRECTNESS/QUALITY` and `INCOMPARABLE`
with a nonzero exit, regardless of a claimed clean context. Diagnostic ablation
measurements remain available in retained results, never correctness evidence.

Labels can coexist. Before all runs complete, output says comparison pending;
no provisional measurement is accepted. Logs are bounded to 16 MiB per run,
plus the small terminal qualification record. Input/restore state remains a
repeated workload even when the relative wall-time gate passes.

Example diagnostic attribution: add `--diagnostic --env 'BB_PASS_TRACE=10+3
BB_PASS_TRACE_OUT=/absolute/path/trace.jsonl'`. Never combine its timing verdict
with the separate unperturbed relative control.

Disk policy is decimal 35 GB floor plus a 5 GB margin, with declared peak growth. The capture
writer requires its estimate plus 40 GB free; runner admission budgets 5 GB, requiring 45 GB.
Use the current approved heavy.sh guard; this tool never lowers or bypasses it. No new capture is
needed for the initial qualification. Captures, unique failure evidence and pinned provenance
must be preserved and coordinated with Sol-Build before any disk changes.

## Legacy host identity assertion

Current protected input: `out/replay/frame-20261012-123949-77481`, produced by c7bd672 after the
da8ab9e quiesced-restore fixes. It has 73 guest submissions, 357 saved images, 280 reset ranges
(one host), 4,851,940,926 logical payload bytes. Its pixels are unqualified. Config was preset 0,
1920x1080 render output, upscaler off, object motion off, 960x540 window. Six logged refreshed
targets are 1920x1080. This supports attribution of internal 1080p work, not full-quality native
presentation acceptance. The older unsafe ceiling capture predates the restore fix and is not
this input. Current replay still carries shader-written buffers/images/cache state between
iterations: it is a repeated workload, not a fully reset frame.

Normal loading refuses v2 reset identity because allocator caller offsets move between builds.
`replay_identity.py SOURCE --output NEW_DIR --identity-map MAP.json` creates a small overlay with
symlinks to unchanged payloads. It never rewrites/deletes the source or copies multi-GB backing.
Run in a heavy slot after release. The map must supply exact SHA256 of the source manifest and
resets, and an `objects` list. Each entry has `old` fields start/size/ordinal/module/offset,
`new` fields name/start/size, and nonempty `evidence`. Integers may be decimal or 0x strings.
Every host reset must match exactly one old identity and fit the asserted named range; every
assertion must be used. Duplicate/overlapping names, missing evidence, truncation and trailing
bytes refuse before output. The manifest, written last, records all source payload hashes, the
original and transformed reset hash, the assertions and their evidence. It is an explicit
semantic identity assertion, not fresh capture authentication.

PM4 recovered matching producer disassembly: pinned libbbgpu SHA256
7e1e61c68c15f355cc16d957b158d35e52ec87beb81b2a5ee8421a660238849d;
GetPort(1) returns driver+0x138, GetBufferLabelAddress adds0x1b0 and returns16 labels.
Thus the named range is 0x80593c328+0x80 in old object 0x80593c040+0x680, ordinal1,
libbbgpu.dylib allocation return offset0x2f8180. Last DCB submission72 packet905 is WRITE_DATA to
that label followed by PatchedFlip0x68750776. This is stronger evidence than address coincidence.
Raw proof is retained in parent/PM4 thread storage and should be named in the assertion.

## Required execution gates

Before accepted relative timing: source review plus full `build.sh --test-keep-going`, clean
cross-build positive with exact image round trip and repeated work counters, and loud controls
for oldv2, truncated/trailing/overlapping/out-of-range records, missing/changed/moved name/size,
and an invalid LAST host/guest range proving earlier reset sentinels unchanged. The production
loader/Restore path must execute; helper-only tests do not qualify it. Preserve each failure.
Then run one explicit `--diagnostic` BB_PERF + BB_PASS_TRACE=10+3 + ring/refresh
attribution, and a separate unperturbed GPU-done wall control. Retain
draw/dispatch counts, guest submissions, actual draw scheduler flushes, Vulkan
submission counts, recorder scopes/on-CPU, prepared draws and upload deltas as separate facts.
No accepted cross-build/runtime or pass/ring timing gate has been completed at source delivery.
