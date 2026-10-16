# Native 1080p quality qualification

`upscaler=fsr3`, `preset=0`, `output_res=1920x1080` selects Native AA at a 1.0
ratio. At this output, `scripts/patches.py::scaled_sizes` returns no startup
resolution patch. Thus `live_resolution=0` still uses the live host-target path:
`BB_RENDER_RES` stays unset and `SceneResolution::ForPreset(0)` is 1920x1080.
The game's existing half-resolution effects remain unchanged. Preset 1 at this
output is 1280x720; preset 3 is 960x540. Neither is native 1080p. At 4K output,
the startup-patch preset 3 deliberately uses 1916x1078 to distinguish scene/UI.

Keep the established FSR3, sharpening, jitter, object motion, effect and LOD
settings explicit in the versioned acceptance preset. No diagnostic preset,
upscaler, resolution, patch or toggle override is acceptance evidence. External
patches run after built-in patches: retain the selected patch list and actual
`patches.bin` hash as well as the ini and executable hashes. MetalFX planning
notes do not establish a working provider; the current Apple path rejects it.

## Bounded source-path evidence

Apple-only `BB_QUALITY_VERIFY=1` emits `QUALITY` records to stderr. Off by
default, it adds no GPU commands, waits, image copies or shader changes. Each
owner-thread stream emits its first observation, state changes and every 120th
observation, capped at 1024 detail records. The per-draw `scene` stream emits only
first/change records; the per-frame counters provide its heartbeat. `exhausted` and destructor `summary`
records expose truncation; no data after an exhausted stream qualifies. Missing
terminal records or an unreported tail are incomplete evidence: qualify only a
closed interval bounded by detail records. Observation sequences are local to
each stream, not shared frame identities or FIF measurements.

- `scene`: the existing main scene-color recognizer's native allocation and
  selected raster/proxy dimensions. These are target extents, not a measurement
  of every draw's viewport/scissor. The existing recognizer, not a census of all
  guest effects, defines this stream.
- `dispatch`: count of successfully recorded FSR3/TAA operations, actual provider,
  render/output dimensions and HDR/LDR path. This runs after the recording API
  returns success, not after resource allocation. It proves command recording,
  not GPU retirement or pixel correctness. Apple FSR4/MetalFX remain rejected.
- `frames`: completed display-pass frame boundaries, cumulative scene, recorded,
  failed, inactive and scene-without-dispatch counts, plus the most recent state.
  A previous success cannot hide later failures/skips. Menu/loading frames may
  have no scene; inspect gameplay-only intervals and explicitly explain gaps.
- `copy`: presenter source and intermediate frame sizes, display override and
  legacy FSR1 state, before the presenter's post-processing copy.
- `present`: intermediate frame and swapchain dimensions at blit recording,
  including resize changes. This does not prove physical display completion.

For native acceptance require scene/raster, recorded FSR3 render/output, copy
source/frame and present frame/swapchain all 1920x1080, with no reduced proxy,
no fixed resolution override, no failed/inactive or unexplained missing scene
dispatches in the qualified interval. Require fresh observations spanning that
interval from the frame, dispatch, copy and present streams, with the scene
state-change stream untruncated; a startup line or context allocation is insufficient.
Visuals, full-quality settings, GPU completion and gameplay checks remain
independent requirements. The old 960x540 play_route window is not native-output
acceptance even when its ini says 1080p.

Use this trace for correctness qualification. Its logging can perturb timings;
performance acceptance needs a separately qualified run with tracing off.

## Validation status

Source-only under the 2026-10-15 disk hold. Sampler state-change/heartbeat/cap
controls are in the existing `upscaler-support-test`; not executed yet. Require
that test, exact-tree keep-going, native scene runtime observations and a
deliberately reduced/disabled control after release. The control must reach the
changed path and must be rejected by the acceptance analysis. No runtime PASS
or performance result is claimed.
