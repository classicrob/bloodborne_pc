# Geometry shaders on macOS (emulated)

Metal, and so MoltenVK, has no geometry stage (`geometryShader = false`; no mesh shaders or transform
feedback either). Bloodborne draws with the GCN ES -> GS -> copy-VS pipeline (`VGT_SHADER_STAGES_EN`
= 0xb0) when entering the world. On Apple those draws are emulated the way the PS4 runs them, with
both rings in memory.

## How a draw runs
1. **ES as compute** (`GsEmulationRole::EsCompute`): one invocation per (index position, instance).
   The ES pulls its vertex id from the guest index buffer and writes its ESGS ring record into the
   draw's scratch, instead of exporting to a geometry stage.
2. **GS as compute** (`GsEmulationRole::GsCompute`): one invocation per (input primitive, instance,
   GS instance). It reads the ESGS records of its primitive and writes GSVS records into a fixed
   slot of `VGT_GS_MAX_VERT_OUT` vertices, plus a run of strip indices: one per emitted vertex, a
   restart index after each strip that draws something, the rest of the run padded with restarts.
   Emits past the bound are dropped; a cut after a strip too short to draw rewinds it.
3. **Copy shader as the vertex shader** (`GsEmulationRole::CopyVertex`): an indexed strip draw over
   the runs (restart on). The copy shader reads GSVS records by vertex index and does its own
   exports, so position, misc outputs and parameters, and the VS-only epilogue (depth range, screen
   space, user clip planes), apply exactly as on the hardware.

Fixed slots keep the PS4 output order (input primitive, instance, GS instance, emission order)
without compaction. Ring record dword d is the IR attribute Position0/ParamN component d % 4 in all
three roles; for GSVS, d is the GCN component index. The emulation adds no IR opcodes.

Indirect ES/GS draws (the first one the game issues is `DRAW_INDIRECT`) run a one-thread setup
kernel (`host_shaders/gs_indirect_setup.comp`). It reads the guest's indirect command and writes the
header plus the ES/GS dispatch and strip draw commands, all of which then run indirectly. Their
scratch is a fixed 32 MiB range; a draw needing more, or reading past `INDEX_BUFFER_SIZE`, draws
nothing and sets error words that the renderer checks at every submission and stops on.

## Code
| what | where |
|---|---|
| layout, header, record encoding | `shader_recompiler/gs_emulation.h` |
| roles in the IR (ring loads/stores) | `ir/passes/ring_access_elimination.cpp` (`CopyVertexRingLoads`, GsCompute stores) |
| loud gaps, role buffers | `ir/passes/gs_emulation_pass.cpp` |
| SPIR-V of the roles | `backend/spirv/emit_spirv_gs_emulation.cpp` |
| pipeline selection | `vk_pipeline_cache.cpp` (EsGs branch, `BuildRuntimeInfo` roles), `vk_graphics_pipeline` (`GsEmulation()`) |
| recording | `vk_gs_emulation.cpp` (`RecordGsEmulation`, `RecordGsEmulationIndirect`), hooks in `vk_rasterizer.cpp` |

## Not emulated (stops loudly, naming the gap)
Scenarios other than G, on-chip GS (ESGS in LDS), stream-out, GS instancing, per-stream output
types, point-list output, input topologies other than point/line/triangle lists, tessellation +
GS, multi-draw and count-buffer indirect ES/GS draws, indexed ES/GS draws with primitive restart,
index swap modes, and, in the ES/GS themselves, `s_barrier`, LDS and `v_writelane`.
`BB_GS_CENSUS=1` (diagnostic) logs each such configuration once, with its binaries in
`<shader dir>/gs_report`, and skips those draws instead of stopping.

## Cost
Two dispatches and two barriers per draw, plus the ring traffic (ESGS + GSVS records written and
read once). As built, a GS draw inside a render pass ends the pass; `BB_FRAME_STATS` prints
`Geometry emulation: draws/s, dispatches/s, render passes ended/s, scratch MB/s`. On the M4 Max a
pass split costs about 0.2 ms at 1080p and 0.3 ms at 1440p when every tile is drawn on both sides
(`tests/macos_gs/bench`). Moving the compute passes in front of the render pass is the next step.

## Tests
`tests/macos_gs/run.sh` (in Darwin `--test`):
- compile-negative controls;
- conformance: synthetic GCN ES/GS/copy programs through the production recompiler on MoltenVK,
  recorded as the renderer records them, compared bit-exactly with a CPU model of the GS (index
  runs, ESGS/GSVS records, and the image against the same primitives drawn as a list, which checks
  primitive order and the provoking vertex), direct and indirect;
- three negative controls that must fail.

`tests/macos_gs/compile_roles` compiles a dumped game triplet (`gs_report`) in the three roles.
