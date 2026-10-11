# MetalFX in the renderer (status and units)

Decisions (orchestrator, 2026-10-10): MetalFX **temporal** is the Apple default upscaler
(`upscaler=metalfx`); FSR 3.1 and TAA stay user-selectable. Nothing switches automatically:
`metalfx` on another platform, or MetalFX unsupported or failing on Apple, is a hard error.

The binding contract is the "Renderer integration proposal" in `metalfx.md` (Opus-Build, agreed
with the renderer): linear HDR input at the HDR post-combine hook before exposure/tonemapping, an
output-resolution tier for the post passes after it, exact active extents, exportable resources
(no copy-based interop), ownership transfers through `VK_QUEUE_FAMILY_EXTERNAL`, a pending
external wait on the next submission, history epochs, an initialized MetalFX-specific reactive
mask, RGB-only reconstruction with the game's alpha preserved. The module is
`gpu/shim/macos_metalfx.{h,mm}`.

## Units

1. **Groundwork (done, edb101c).** VK_EXT_metal_objects required on Apple; the instance is
   created exportable as a Metal device. `Scheduler::WaitBeforeNextSubmit` (consumed by the next
   submission, all stages; a full SubmitInfo is a hard error) and a stage per wait.
   `upscaler=metalfx`: a startup error on Apple until the provider is complete, an error
   elsewhere, disabled in the menu.
2. **Build.** `gpu/shim/macos_metalfx.mm` as an Objective-C++ source of bbgpu on Apple, with
   Foundation, Metal, MetalFX and Security (Opus-Build: CMake and the export gate).
3. **Exportable resources.** Upscaler-owned images (motion RG16F, reactive R8, output RGBA16F)
   and the two timeline semaphores created with `VkExportMetalObjectCreateInfoEXT`. The HDR
   scene color (RGBA16F) and the scene depth are texture-cache images: the cache creates the
   images MetalFX reads as exportable when MetalFX is selected (format and usage checked at
   creation; anything else is a hard error naming it). Exact active extents: the module
   rejects padding.
4. **The hook.** At the HDR post-combine hook (`TemporalUpscaler::OnDispatch`, extended from
   native AA to the scaled presets): end rendering, release to EXTERNAL, Flush with producer N,
   `Submit`, `WaitBeforeNextSubmit(consumer N)`, acquire in the next command buffer, with the
   renderer's layout/ownership records updated alongside.
5. **Output tier.** SceneTargets gains an output-resolution tier: post passes after the hook
   read the reconstructed output and write output-size targets; views, coordinate constants and
   extents follow the tier.
6. **Qualification.** Exposure (auto exposure, preExposure 1 while the game's HDR is not
   pre-exposed), history epochs (output/preset/provider changes, missing camera history,
   UI/loading transitions, a verified cut signal), the MetalFX reactive mask, alpha of each post
   consumer, UI. Then BB_PERF at 4K against FSR 3.1, including the 2.25x post-pixel cost of the
   output tier.

`metalfx_supported` becomes true only when units 2-5 are in and unit 6's gates pass.
