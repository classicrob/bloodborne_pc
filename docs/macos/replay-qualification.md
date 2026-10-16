# Replay qualification and existing-build A/B

`replay-ab.sh A_MANIFEST B_MANIFEST --capture DIR --iters 30 --order ABBA`
uses existing artifacts only. Build manifests record a full source SHA and absolute paths plus
SHA256 for `artifacts.bb-probe` and `artifacts.libbbgpu.dylib`; the latter must be the exact
`gpu/libbbgpu.dylib` beside the executable. Shared previews require `qualification.test_gate=PASS`.
`--qualification-only` permits missing/NOT_RUN/SKIPPED gates for correctness diagnosis and marks
results unqualified; FAILED gates are refused. No replay proves live FPS or pixel correctness.

The runner hashes inputs outside timing in a build slot, then launches each run under heavy.sh
with an exclusive GPU, stop-mode CPU quiet and settle (default load below 6, bound 120 seconds).
Each run records host power/thermal/memory/load after settling. A fresh output directory retains
config/input/binary hashes, plans, heavy output, BB_PERF and per-run results. Environment overrides
are parsed as arguments, never shell code; capture/iteration/timestamp/lock settings are protected.
Only unchanged pinned binaries and boot inputs are used. STOP/watchdog, nonzero exit, missing or
duplicate iteration, missing image round trip or missing GPU samples fail the orchestration.
Missing GPU timing is never zero. Busy readback is delayed: it covers iterations 2..N-1, while wall
and CPU summaries cover 2..N. `replay_summary.py --expected N --require-busy --json LOG` validates
that contract independently. Logs are bounded to 16 MiB per run. Keep experimental pass/refresh
traces in their own runs, with a separate unperturbed relative timing control.

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
Then run one BB_PERF + BB_PASS_TRACE=10+3 + ring/refresh attribution, and a separate unperturbed
control. Retain draw/dispatch counts, guest submissions, actual draw scheduler flushes, Vulkan
submission counts, recorder scopes/on-CPU, prepared draws and upload deltas as separate facts.
No accepted cross-build/runtime or pass/ring timing gate has been completed at source delivery.
