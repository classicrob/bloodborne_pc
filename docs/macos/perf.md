# macOS performance measurement

`BB_PERF=1` records coarse performance windows for the macOS build (x86_64 under Rosetta,
MoltenVK). It is off by default. When it is off, the hooks do nothing: no query pools, no
extra submissions, no file. `tools/macos/perf_run.sh` drives the tracked production route
(`tools/macos/play_route.py`) with it, and records the host load next to the results.

| Variable | Values | Meaning |
|---|---|---|
| `BB_PERF` | `0`/unset, `1` | Enable. Any other value aborts. |
| `BB_PERF_OUT` | path | New JSONL file, created exclusively. Required with `BB_PERF=1`. |
| `BB_PERF_PHASES` | `0`/unset, `1` | One submission per phase (below). Requires `BB_PERF=1`. |

`BB_PERF` together with `BB_GPU_PROFILE` aborts. `BB_GPU_PROFILE=1` alone aborts on macOS
(see the next section).

## GPU timestamps on MoltenVK: what is valid

Before any GPU number is trusted, `tests/macos_perf/timestamps.sh` qualifies MoltenVK
timestamps on the actual device. It is part of Darwin `build.sh --test`. On an M4 Max, macOS
27.2, MoltenVK 1.4.2 (queue family 0, `timestampPeriod` 1 ns, 64 valid bits), three runs:

| Timestamps | x1 / x2 / x4 / empty / x1 again (ms) | Valid |
|---|---|---|
| First and last of each submission, compute | 1.39 / 2.79 (2.01x) / 5.66 (4.06x) / 0.004 / 1.40 | yes |
| First and last of each submission, 64/128/256 MiB fills | 0.16 / 0.32 (1.95x) / 0.69 (4.20x) / 0.004 / 0.18 | yes |
| Between compute dispatches in one command buffer | 2.03 / 2.05 (1.01x) / 5.18 (2.56x) / **0.60** / 0.30 (0.15x) | **no** |
| Between fills in one command buffer | 0.15 / 0.30 (2.04x) / 0.64 (4.33x) / 0.003 / 0.17 | yes |

Timestamps written between compute dispatches in one command buffer put work in the wrong
segment. An empty segment absorbs 0.5 to 0.9 ms, and a repeat of the first segment reads
0.15x. The span from first to last stays plausible, and the misattribution showed up on every
run. Inserting a 4-byte fill before each timestamp, which forces a Metal encoder change, made
it worse. Apple GPUs sample counters only at stage boundaries, and MoltenVK maps
`vkCmdWriteTimestamp` onto those samples. So a timestamp recorded among other encoders does not
mark the point in the command stream where it was recorded.

The first and last timestamp of each submission bracket that submission's work. They scale
with the work, an empty submission reads about 4 µs, and serialized submissions are ordered on
one timebase. That is the only contract BB_PERF relies on. The test asserts it and prints the
in-buffer behavior as a diagnostic. If a later MoltenVK changes either result, the test
output shows it.

`build.sh --test` shares the GPU with whatever else is running, often a game. So the test
takes the fastest of 11 samples per segment, since interference only adds time. Its scaling
windows are wide: x2 between 1.4 and 2.8, x4 between 2.6 and 5.6 (6.5 for compute: long
compute runs ran 4.0 to 5.4x under sustained load while fills stayed at 3.9 to 4.2x), and the
repeat between 0.6 and 1.6. The empty segment must stay under 0.1 ms and under 5% of the unit. That check is
what separates the valid case (about 0.004 ms) from the invalid one: in-buffer compute gave
0.26 to 2.7 ms, with repeats of 0.0 to 0.22x, over seven runs at host load 20 to 70.

Consequences:
- BB_PERF measures GPU time per submission, for the main (draw) and present schedulers. It
  never places timestamps between commands.
- `BB_PERF_PHASES=1` ends the current submission at each phase boundary, so each phase is its
  own measured submission. This adds a few submissions per frame and changes scheduling, so
  every record of such a run says `"perturbed": true`. Use it for attribution, never for FPS.
- `BB_GPU_PROFILE=1` (per render pass, timestamps inside command buffers) aborts on macOS
  rather than print numbers that are wrong.
- Per-pass GPU time on macOS would need Metal System Trace. Instruments/xctrace must not be
  attached to a translated (Rosetta) process (team rule). Try it first on a native arm64
  Metal test, and ask before using it on the game.

This is worth reporting to MoltenVK together with the push-descriptor `OpArrayLength` repro
(`tests/macos_descriptor_sizes`).

## Output

`BB_PERF_OUT` is JSONL. Its records are:

- `{"type":"schema","version":2,"perturbed":…}` is written once.
- `{"type":"queue","present":…,"timestamp_valid_bits":…,"timestamp_period_ns":…}` is written
  once per scheduler.
- `{"type":"window",…}` is written every 5 s at a guest flip. A last record with
  `"final":true` is written if the renderer is torn down normally. A killed process loses the
  partial window.

A window holds:

- `flips`: the number of guest flips in the window.
- `gpu.main_ms`, `gpu.present_ms`, `gpu.*_submissions`: the sums and counts of submissions
  completed in the window. Results are read when later submissions begin, a few submissions
  behind.
- `gpu.busy_union_ms`: the union of all submission spans on the GPU clock, both streams. It
  is the GPU busy time on the shared graphics queue; overlap is not double counted.
  `busy_union_ms_per_flip` divides it by `flips`, which is the number to compare with the
  16.7 ms budget at 60 FPS.
- `metrics`: distributions with `n`, `mean`, `p50`, `p95`, `p99` and `max`:
  - `gpu_main_submission_ms` and `gpu_present_submission_ms`: one sample per submission.
  - Per guest flip: `guest_flip_interval_ms` (includes pacing and blocking),
    `liverpool_cpu_ms` (Mach thread CPU of the GPU command thread), `recorder_chunk_cpu_ms`
    and `recorder_chunk_count`, and the wall time spent waiting:
    `liverpool_idle_wall_ms`, `fence_wait_wall_ms`, `host_copy_wait_wall_ms`,
    `copy_worker_wait_wall_ms`, `record_sync_wait_wall_ms`, `import_growth_wait_wall_ms`
    and `import_growth_wait_count`.
  - Only with `BB_PERF_PHASES=1`: `gpu_scene_ms`, `gpu_post_ms`, `gpu_upscaler_ms` and
    `gpu_frame_work_ms`, one sample per guest frame. A frame's last phase is recorded when
    the next frame's first submission is read.

Phase boundaries are `Rasterizer::NoteFrameStart` (scene), the upscaler's trigger dispatch
(post), and `TemporalUpscaler::Run`/`RunScaled`, which mark the upscaler on entry and return to
the previous phase on exit. The present scheduler is its own stream.

## Running

```sh
tools/macos/perf_run.sh --probe <shared>/out/bb-probe --output 4k [--phases] [--label NAME] \
    [--until STAGE --hold SECONDS]
```

`--hold` keeps the game at the route's last stage for that long and then stops it. The summary
then covers only the windows completed during the hold, so loading on the way is excluded.

The script refuses to run while another `bb-probe` is running: the run needs the GPU to
itself, and that also keeps within the team's limit on translated processes. It records `uptime`, the other translated processes, the commit, and the
`bb-probe`/`libbbgpu.dylib` SHA-256s before and after the route. Then it prints a summary of
`perf.jsonl` and fails if the file has no window. Numbers from a loaded host are labelled
with that load, and they are not a clean benchmark.
