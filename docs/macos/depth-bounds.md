# Depth bounds test on macOS

M4 MoltenVK has no `depthBounds`. Bloodborne uses the test after "Finalise contract" (for example
`[0.99954, 0.99963]`). The test discards a fragment when the depth already stored at its pixel lies
outside `[min, max]` (inclusive), before any other per-fragment work, independent of the fragment's
own depth.

## Emulation

Skipping the test is exact when it always passes: no depth buffer, or bounds covering [0, 1].
Stored depth cannot leave [0, 1] on MoltenVK (`FeatureChecks::DepthBoundsUnsupported`). Otherwise:

- **Pipeline (`vk_pipeline_cache.cpp`):** sets `HwFragmentRuntimeInfo::depth_bounds_test`.
- **Fragment shader (Sol-2, recompiler):**
  - fetches the stored depth from a depth-aspect sampled image at `DescriptorSet 1, Binding 0`
    (`OpImageFetch` at `ivec2(FragCoord.xy)`, lod 0);
  - kills the fragment when the depth is outside `[push.depth_bounds_min, push.depth_bounds_max]`
    (byte offsets 320/324);
  - does this before any guest instruction, and without early fragment tests, so a killed fragment
    makes no stencil or depth update, no storage write, and no occlusion count.
- **Renderer (`Rasterizer::BindDepthBoundsSource`):**
  - The draw keeps the depth/stencil image attached in a read-only depth layout (stencil stays
    writable if the draw writes stencil) and samples that same image. There is no copy.
  - A render pass breaks only when the attachment switches between writable and read-only.
  - The bounds come from `DB_DEPTH_BOUNDS_MIN/MAX` in the push data.
- **Evidence:** `tests/macos_depth_bounds/probe.cpp` on M4. The stored 0.25 was killed and 0.75
  kept and fetched exactly. With stencil writable, the killed pixel kept its stencil and the kept
  pixel wrote it.

## Hard errors

- A depth-bounds draw that writes depth. A same-pass read cannot follow writes made within the
  draw.
- An MSAA depth buffer (needs a per-sample fetch).
- A pipeline without a fragment shader.
- A reduced-size scene depth proxy as the attachment.

## Tests

- **`tests/macos_depth_bounds/probe.cpp`:** feasibility of same-pass read-only sampling on
  MoltenVK.
- **Opus-Verification group `depth-bounds`:** inclusive edges, `min == max`, bounds between stored
  values, `min > max`, trivial bounds, and D16.
