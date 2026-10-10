# Device feature audit (M4 Max, MoltenVK 1.4.2, macOS 27.2)

This audit covers every Vulkan feature the renderer can depend on that M4 MoltenVK reports as
absent (`out/m4-moltenvk-full.txt`, Sol-1's device report), and how the Apple path handles a guest
use of it. "Exact" means the Apple result equals the PS4/Linux result. "Hard error" means the first
use aborts with a message naming the state. A missing feature is never silently skipped. The
per-draw checks evaluate the pure predicates in `vk_feature_checks.h`;
`tests/test_feature_checks.cpp` runs each on the legitimate state next to the unsupported one.

| Feature (M4) | Guest use | Apple handling | Where |
|---|---|---|---|
| geometryShader | ES/GS stages, on-chip GS, stream-out | Hard error | `vk_pipeline_cache.cpp` RefreshGraphicsStages |
| logicOp | `CB_COLOR_CONTROL.ROP3` other than copy, on a bound, written UNORM/SNORM/integer target (Vulkan and GCN ignore the logic op on float and sRGB targets; CB mode Disable leaves no bound target) | Hard error (formerly dropped silently: the draw blended normally) | `vk_graphics_pipeline.cpp` |
| depthBounds | `DB_DEPTH_CONTROL.DEPTH_BOUNDS_ENABLE` | Exact when the test always passes: no depth buffer, or bounds covering [0, 1]. Stored depth cannot leave [0, 1] here: MoltenVK has no `VK_EXT_depth_range_unrestricted`, so viewport depth ranges and fragment depth are clamped to it. Otherwise a hard error (formerly the test was skipped) | `vk_rasterizer.cpp` UpdateDepthStencilState |
| drawIndirectCount | `DRAW_INDEX_INDIRECT_COUNT_MULTI` with a count address | Hard error (formerly called anyway: invalid Vulkan). An exact path would be a compute pass that zeroes the arguments past the GPU count, then a plain multi-draw-indirect (`multiDrawIndirect` is supported) | `vk_rasterizer.cpp` DrawIndirectRecord |
| wideLines | lines reaching the rasterizer (line primitives, a GS emitting line strips, isoline tessellation, or triangles with either face in line polygon mode) with width ≠ 1 | Hard error; widths below 1 too, as GCN's thin-line rasterization is not known to match. Width is forced to 1 otherwise, the only valid value; it has no effect on other draws. Guest init sequences program width 1 (`PA_SU_LINE_CNTL` = 8) | `vk_rasterizer.cpp` UpdateRasterizationState |
| (one polygon mode) | `PA_SU_SC_MODE_CNTL` front and back polygon modes differ with back faces not culled | Hard error: Vulkan has one polygon mode and the front mode is used | `vk_rasterizer.cpp` UpdateRasterizationState |
| shaderStorageImageMultisample | storage view of an MSAA image | Hard error | `image_view.cpp` |
| shaderBufferInt64Atomics / shaderSharedInt64Atomics | 64-bit atomics | Recompiler hard error | `emit_spirv.cpp` SetupCapabilities |
| shaderFloat64 | F64 ALU | Recompiler hard error (Sol-2) | `LowerFp64ToFp32` |
| robustBufferAccess2 | out-of-range buffer access | Exact: recompiler bounds emulation (Sol-2) | `docs/macos/buffer-robustness.md` |
| nullDescriptor | unbound/null resources | Exact: dummy zero resources | `75ba0b2`, `test_null_descriptors.cpp` |
| vertexInputDynamicState, fixed attribute formats | vertex fetch | Exact: shader vertex pulling (Sol-2) | `emulate_vertex_fetch` |
| sparseBinding / sparseResidency* | guest memory arenas | Exact: host-import arena, no sparse | `docs/macos/host-import-arena.md` |
| D16_UNORM_S8_UINT / D24_UNORM_S8_UINT (formats) | Z16 + S8 depth | Hard error (pending exact design) | `docs/macos/format-support.md` |
| Push descriptors (buggy `OpArrayLength`) | guest buffer bindings | Exact: allocated descriptor sets | `c4f224b` |
| MAILBOX present mode | presentation | FIFO by design | `docs/macos/present-mode.md` |
| shaderCullDistance | VS cull distances | Not used: `needs_clip_distance_emulation` is NVIDIA-only and cull distances are not exported. Unaudited beyond that | — |
| pipelineStatisticsQuery | guest pipeline-stat queries | Unaudited | — |

## Open items (not yet correct or loud)

- **Primitive restart (Metal behaviour).** Stock MoltenVK cannot disable restart for strips and
  fans, and it never restarts list topologies (`MVKPipeline.mm`; `VK_EXT_primitive_topology_list_restart`
  needs the private Metal API). Results differ from the PS4 only when an index buffer actually
  contains 0xFFFF/0xFFFFFFFF: a strip drawn with restart off, or a list drawn with restart on. An
  exact check would have to scan index data, so this needs a design (for example a GPU scan that
  compacts restart-cut lists). Today's existing assert covers only restart indices other than -1.
- **Tessellation (recompiler, Sol-2).** Isoline domains are emitted although
  `tessellationIsolines` is false. Point-mode output is never emitted, on every platform. Pow2
  partitioning warns and uses equal spacing. Each should be exact or a hard error.
- Shader float-control modes (denorm/RTZ) are covered by `docs/macos/float-modes.md`.
