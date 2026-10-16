# Native 1080p acceptance plan

Status, 2026-10-15: 15 deterministic Python tooling tests passed; C++ execution,
shared-preview and runtime acceptance are pending. The lead lifted the general
disk hold under the user's new 35 GB decimal floor. No new measurement, capture,
build or gameplay result is claimed. The 4K baseline queue is retired. Goal
remains sustained 60 FPS with existing quality.

## Exact preset and output evidence

`tools/macos/playtest/native-1080p.ini` explicitly pins existing settings:
FSR 3.1 Native AA, preset 0 (scale 1.0), 1920x1080 output, sharpening on at
0.30, jitter and object motion on, reactive mask off, game model LOD 0.
Chromatic aberration, DoF, motion blur, SSAO, game AA and dynamic shadows retain
their enabled defaults. Added SSR, cheats and tweaks retain disabled defaults.
Disabling a default effect or reducing internal resolution is not an accepted arm.

`play_route.py route --native-1080p --probe <signed-preview>/bb-probe
--preview-manifest <signed-preview>/manifest.json
--fixture <scene> --until world --keep` selects this configuration, requests a
1920x1080-point background window and requires its logged pixel size to be
1920x1080. The default tool mode remains 960x540. On a display where points and
pixels differ, a failed preflight requires explicit window sizing work; it is
not a reason to accept a different output size. The option refuses ini,
unreviewed diagnostic and nonzero initial optimization-mask overrides. It uses
an empty external patch directory/selection and records generated patches.bin
and configuration hashes before releasing the launch lock.
Native mode requires the shared preview manifest's executed `test_gate=PASS`,
full source SHA and exact stage paths/hashes for the verified probe/library pair
before pinning any artifacts. Its byte hash and source metadata are retained in
the route manifest; staged artifacts need not reside in a git checkout. Signing
alone cannot qualify a preview. A non-PASS preview may still be used in generic
diagnostic mode without making a native acceptance claim.

`live_resolution=0` at 1080p still uses the host live-target path: the startup
patch generator returns no scaled size for the game's native output. Expected
startup log is `Output resolution: 1920x1080 (live)`. At this output preset 1
would be 1280x720 internally and preset 3 would be 960x540; both fail. Historical
4K preset-3 claims are irrelevant to this target and are not a baseline.

The window check alone does not qualify the full chain. Before accepting a
run, require Renderer/Shaders' bounded `BB_QUALITY_VERIFY` evidence of actual
successful FSR dispatch, input/output extents 1920x1080, scene raster dimensions,
and completed display-source/copy/swapchain dimensions. A context-allocation
message is insufficient. Verify no failed/disabled/missing path, later resize,
unknown patch, effect change or quality-changing toggle. Full FPS runs omit
quality/flip/resource tracing after the same exact artifact/config passes the
correctness gate. Record every deliberate PM4 diagnostic arm separately.

Historical logs such as COMBAT-1 and COMBAT-2 report 960x540 window pixels.
Their gameplay observations remain useful, but they cannot qualify native
1920x1080 presentation output. Screenshot dimensions include window decoration
and compositor scaling and cannot replace render/swapchain extent evidence.

## Release, preservation and quiet run prerequisites

The lead has released the general disk hold. Coordinate named jobs with Build;
the first priority is Replay's incremental build, loader negative/cross-build
qualification and small attribution run, then the shared preview in a permitted
slot. Do not duplicate that build or create new captures before capture
reuse/migration is qualified and budgeted. Live acceptance requires the signed
shared preview and executed full-gate summary. Check raw available bytes before
every job that can grow files. Budget measured peak growth above the hard floor
of 35,000,000,000 bytes plus the default 5,000,000,000-byte safety margin: a
1,000,000,000-byte job needs at least 41,000,000,000 bytes available, including
other active reservations. An older 50 GB floor or 60 GB target is superseded.
Refuse an unbudgeted capture/build. Do not duplicate the user's broader cleanup.

Use current macos `heavy.sh`, one heavy job per worktree, exclusive GPU,
`--cpu-quiet` stop mode and `--settle 4,30` agreed with Build. Retain top-process,
thermal, power/memory-pressure and load records. E-core quiet mode does not
qualify timing. Do not edit/switch under a job, sample a Rosetta process, or
stop another owner's PID. Reuse `out/macos_route/data-1080p`; do not create a
4K data tree. Preserve original saves and immutable fixture ancestry. Fresh
run users receive copies; never write the source fixture in place.

Keep compact frame intervals/provenance, bounded screenshots and unique failure
logs/dumps. Existing protected evidence and fixture hashes are indexed in
the Playtest thread storage's `protected-evidence.json` and `fixture-status.json`.
Neither index replaces or authorizes deletion of originals.

## Ordered experiments after release

1. Check preview signatures/hashes, configuration/patch hashes and actual native
   dimensions; establish safe scene framing with quality checks enabled.
2. Warm each scene using the exact build until route shader compilation settles.
   Any compilation during a timed warm segment is reported, never silently
   discarded. Keep separate cold-cache startup/compilation evidence.
3. Take an initial quiet warm baseline: clinic, Central Yharnam traversal and
   combat, at least 180 seconds per representative scene. Idle clinic is an
   attribution/control scene, not traversal/combat acceptance. Repeat at least
   three sustained representative runs before claiming final performance.
4. Repeat matched scenes with BB_PERF absent (also no BB_PERF_FRAMES/PHASES),
   using ordinary Frame stats counts and elapsed time as an independent
   non-BB_PERF guest-FPS check. This is not an actual-present oracle. Replay
   has not recovered a separate actual-display-present FPS counter; that
   acceptance gap remains explicit. Compare instrumentation overhead.
5. On the PM4 candidate, run clinic and Central Yharnam with BB_PIPE_VERIFY=1
   and BB_QUALITY_VERIFY: zero changed-resource-table lines, existing ordering/
   mutation controls and correct visuals. Live bit-4 changes must execute.
   The lead parked new frame-overlap telemetry; no BB_FLIP_TRACE is in the
   queued preview. Existing intervals/GPU busy permit a throughput comparison
   but do not establish coherent queued/submitted/not-retired overlap. Keep
   that evidence gap explicit; do not hold the first comparison on new tracing.
6. Only after the runtime gate passes, quiet live ABBAABBA comparisons with
   identical fixtures/cache/quality and matched visually supervised inputs.
   Word2 bit 4 restores the old stage-B wait only; compare a pre-change
   executable as well. Include the explicit BB_FRAMES_AHEAD=2 arm. Agree masks
   and diagnostic allowlist with PM4; no arbitrary disabled optimization mask.

Report exact guest-interval median/p95/p99, 1% low, >60-Hz-budget, >50-ms and
>100-ms counts, maximum, duration/sample count, compilation and missing/invalid
data, plus independent guest FPS and GPU busy per flip. Never pool window
percentiles or count synthetic/repeated frames as a performance win.
Missing final-window data cannot qualify an unbounded full run; retained bounded
segments must cover the specified end and disclose the missing tail.

## Routes and gameplay gaps

No unattended movement based only on coordinates. `goto` calibrates by moving
in two directions and previously walked off the Central Yharnam balcony.
Fresh telemetry does not prove a traversable path. Use captures to establish
ground/ledges/obstacles, short bounded inputs with visual rechecks, and position
telemetry only as corroboration. Historical `r0.sh`, pan and arch timed loops
are not safe native-acceptance traversal scripts. Do not leave a live game
unattended in the system menu: it does not pause enemy attacks.

Retained concrete BC4 trigger, PREVIEW-28p: load `central-yharnam-combat-edited`,
reach world, inspect spawn, press options 0.12 s, right 0.12 s, cross 0.12 s
(approximately 2 s between presses, checking the screen after each). COMBAT-1
stopped with `Bc4UnormBlock` storage view at 0x70e5c3af00. No movement preceded
that stop. The four later PREVIEW-28bc status/walk/menu attempts exited cleanly;
they do not prove the diagnostic or corrected alias path executed. A separate
historical street/camera trigger still needs exact identity recovery. On a
diagnostic build, preserve full requested view/effective T#/shader/resource/
is_r128 output and prove the corrected path executes before calling it fixed.

Existing fixture evidence qualifies first enemy damage/retaliation, death and
in-process Central lamp respawn; the combat edit additionally qualifies taking
damage, vial consumption 20 to 19, healing, attack, normal save and fresh-process
reload with level40/HP1100/VIT30/STR23 and 19 vials/19 bullets on PREVIEW-28bc.
These are edited fixtures, not played-through progression. Fresh-process
lamp-lit interaction and Pebble persistence remain open, as do general quest/
lamp progression and physical controller/audible audio acceptance. Recheck
required gameplay on the final candidate and enumerate every untested scene.
