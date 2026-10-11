# Shader corpus sweep (Apple)

`tools/macos/shader_sweep` compiles every shader the game ships through the production Apple
shader path, offline, and reports which ones stop. It finds recompiler gaps (wave64 emulation,
geometry emulation, float controls, push user data) without reaching the draw in game.

```
ninja -C out/gpu shader-sweep
tools/macos/heavy.sh --pool build "shader sweep" -- \
    python3 -I tools/macos/shader_sweep/sweep.py --game "$BB_GAME_DIR" --out out/shader_sweep
```
Options: `--gpu-build DIR` (default `out/gpu`), `--only HASH,...`. The game data is only read.

## What it runs
1. `extract.py` decompresses `dvdroot_ps4/shader/*.shaderbnd*.dcx` (DCX zlib, BND4 binders;
   `fromsoft.py`) and parses each `.?po` file: the Gnmx stage struct (stage registers, input
   usage slots, vertex semantics), the GCN code with its `OrbShdr` binary info, and for a GS its
   copy shader. The hash is the binary info's `shader_hash`, the one the game log and
   `BB_DUMP_SHADERS` print. One job per distinct (hash, stage); a GS gets one job per ESGS item
   size of the ES shipped under its name.
2. `out/gpu/shader-sweep` (`sweep.cpp`, linked against `libbbgpu`) creates the device and the
   production `PipelineCache` (so the profile is the device's Apple profile) and calls
   `GetProgram` per job the way `RefreshGraphicsStages`/`RefreshComputeKey` do: CS, VS, PS,
   ES as compute, GS (after the production `GsEmulationUnsupported` check) plus its copy shader,
   HS, DS (VS after an HS) and LS (against a compiled HS). Registers come from the stage struct;
   user data is built from the input usage slots as the Gnm driver fills it: V#/T#/S#
   descriptors, resource tables (2D RGBA8 T#s), constant buffer and ring tables (float4 V#s),
   extended user data in memory, and a fetch shader generated from the vertex semantics.
3. `sweep.py` runs the driver in at most two processes; a job that stops the process
   (assertion, exit 23) or crashes is recorded and the driver restarts after it. Every SPIR-V
   module (`BB_DUMP_SHADERS` output) goes through `spirv-val --target-env vulkan1.3`.

## Output (`out/shader_sweep`)
- `results.tsv`: hash, stage, PASS / LOUD (the stop message) / CRASH / HANG / SETUP (the sweep
  could not build the job), spirv-val result, errors and warnings logged by the job, file name.
- `report.md`: counts per stage, everything not PASS grouped by message, spirv-val failures,
  logged errors.
- `gs_census.md`: the static GS census: each ES/GS/copy configuration (input topology from the
  GS's ESGS ring loads, output primitive, max vertices, per-stream vertex sizes, instancing,
  stream-out, ESGS size and ES hashes) with the production verdict.

## Limits
A PASS means the shader compiles with the registers in its file and representative draw-time
state: render target formats from the export formats, clip space 0..W, one tessellation patch
per group, the texture and buffer types above. Shaders whose code depends on descriptor contents
(texture type, buffer format) or on registers the file does not carry can still stop in game
with other state; the in-game route remains the check for those.
