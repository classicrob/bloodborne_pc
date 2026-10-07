# MetalFX feasibility on the Rosetta macOS port

Tested 2026-10-07 on M4 Max, macOS 27.2 (26B5101f), macOS 26.4 SDK,
LunarG Vulkan 1.4.363 / MoltenVK 1.4.2. This is a standalone prototype,
not a renderer provider or game acceptance result.

## Result

**Go for further integration research.** In an actual x86_64 Rosetta process:

- Both temporal and spatial descriptors return supportsDevice=YES, create
  scalers, encode work and produce checked pixels.
- VK_EXT_metal_objects exports the device, textures and timeline semaphore
  MTLSharedEvent objects. MetalFX uses the original Vulkan-owned textures.
- Vulkan renders a constant-color input; MetalFX executes on a separate Metal
  command queue; Vulkan reads the original output image afterwards. Sixteen
  alternating-color frames pass for both D32F and D32F/S8 depth configurations.
- A further temporal encode reaches a Vulkan swapchain and vkQueuePresentKHR
  succeeds. The only full-image blit is presentation into the swapchain;
  there is no Vulkan-to-Metal or Metal-to-Vulkan texture copy. Pixel checks
  use a one-pixel diagnostic readback outside measured work.

Temporal creation logs Apple Neural Engine Code=26, reporting a model compile
bad argument, even with synchronous initialization requested. The scaler is
nonnull and its GPU encodes/pixel checks pass. These observations do **not**
identify which implementation MetalFX selected internally or establish whether
the message affects quality or performance. Qualify that driver/framework error
before treating this as a production provider. The prototype does not install
a substitute scaler when an API or pixel check fails.

Apple describes the APIs in its
[MetalFX overview](https://developer.apple.com/documentation/MetalFX).
Rosetta support above is observed, not inferred from that page.

## Reproduce

Use the existing shared x86_64 dependency prefix and already-built FSR libraries:

~~~sh
BB_DEPS_PREFIX="$HOME/bbport-deps/x86_64" bash tests/macos_metalfx/run.sh
BB_DEPS_PREFIX="$HOME/bbport-deps/x86_64"   bash tests/macos_metalfx/run.sh --depth-stencil --present
~~~

The optional --present briefly creates a 640 × 360 SDL/Metal window. Scaling
remains 2560 × 1440 to 3840 × 2160; the presentation blit fits that output to the
window. This does not test a 4K physical display or the MacBook panel.
BB_FSR_STATIC_DIR can select the existing shared build's
out/gpu/third_party/fsr-vulkan. No second GPU tree is built.
Missing libraries, unsupported features, failed commands and incorrect pixels
exit nonzero. Tests run sequentially without debugger/sampling tools.

support.mm is the smaller descriptor-creation probe. interop.mm is the actual
encode/interop/presentation/benchmark program. Scratch is approximately 6 MiB
in out/macos-metalfx. GPU resources have process lifetime; process teardown
releases them. No game asset is read.

## Interop contract tested

The prototype declares export intent with VkExportMetalObjectCreateInfoEXT at
instance, image and semaphore creation. It exports VkImage-backed MTLTexture
objects directly, rather than using IOSurface or staging a second texture.
Output is private-storage RGBA16F with MetalFX's requested read/write/render
target usage bits. The depth/stencil case uses the base D32F/S8 Metal texture,
with no depth conversion copy. See Khronos'
[metal object export rules](https://docs.vulkan.org/refpages/latest/refpages/source/VkExportMetalObjectsInfoEXT.html).

Each conformance frame follows this sequence:

1. Vulkan acquires resources, renders color, initializes depth/motion, releases
   ownership to VK_QUEUE_FAMILY_EXTERNAL, then signals timeline A.
2. The separate Metal command buffer waits on A's exported shared event,
   encodes MetalFX on the exported textures, then signals B's shared event.
3. Vulkan waits on B, acquires the output, checks it via Vulkan transfer/readback,
   and releases it again for the next Metal encode.

The host waits on the final Vulkan fence to bound and verify each test frame.
It does **not** wait between Vulkan producer and MetalFX, or between MetalFX and
Vulkan consumer. Inter-API ordering is GPU-side, not supplied by a hidden CPU
wait. Separate timelines avoid sharing one value between independent producers.
The [extension design](https://docs.vulkan.org/features/latest/features/proposals/VK_EXT_metal_objects.html)
covers sharing Metal resources and synchronization with Vulkan.

Production needs a submission boundary before MetalFX, a Vulkan submission
waiting on its event afterwards, and retention of textures/scalers until work
retires. This adds two event operations and submission/ownership bookkeeping per
frame. Scheduler integration overhead is not measured here. There is no
per-draw work or inter-API pixel copy.

vkGetMTLTextureMVK is unnecessary on this tested path. The inspected MoltenVK
MVKDevice::exportMetalObjects implementation returns the image's Metal texture
and semaphore's shared event directly. A future provider should use this one
tested EXT path, not silently change APIs or stage a copy on export failure.

## Mapping the existing FSR inputs

Current code, rather than older planning entries in [upscaler.md](../upscaler.md),
is authoritative: vk_temporal_upscaler.cpp, host_shaders/camera_motion.comp,
vk_camera_motion.cpp and the portable FSR context contract.

| Input | Existing bbport path | MetalFX mapping and qualification |
| --- | --- | --- |
| Color | Native AA uses RGBA16F HDR before post; scaled output uses the tonemapped frame with FSR HDR/auto-exposure flags disabled. | The prototype validates linear HDR RGBA16F only. The scaled LDR path is a **mismatch to resolve**, not a proven drop-in input. Establish transfer function/insertion point; use a compatible linear texture/view where valid, or explicitly account for conversion cost. |
| Depth | D32F or D32F/S8, normal 0-near/1-far projection; near about 0.05, far 3000. | Both formats encode successfully. Set depthReversed=NO; no inversion copy. MetalFX has no matching camera-near/far parameters; keep device depth rather than substituting FSR linearized depth. |
| Motion | Render-resolution RG16F: (previous_uv-current_uv)*render_size; X right/Y down. Camera shader removes current viewport jitter; object vectors use the same pixel convention. | Set both motionVectorScale components to 1. Apple's previous-pixel convention agrees with the source. Nonzero camera/object motion and disocclusion quality remain untested in this zero-motion fixture. |
| Jitter | Centered Halton(2,3), ±0.5 render pixels, scale-dependent phase count. FSR defaults to positive X-right/Y-down viewport-shift components. | Use render pixels, not UV/output units. Zero jitter does not prove MetalFX's sampling-offset sign relative to Vulkan viewport shift. A grid/motion test must determine and lock the sign; no sign-flip heuristic ladder. |
| Exposure | HDR uses auto exposure, no explicit exposure texture, preExposure=1. Scaled LDR uses neither HDR nor auto-exposure flags. | HDR prototype uses autoExposureEnabled=YES, preExposure=1. Match the game's tonemapper in quality tests. Explicit MetalFX exposure is a 1×1 R16F texture, not an arbitrary FSR exposure resource. |
| Reset | Changes to output/preset/provider/jitter, missed dispatch and returning from UI/loading to 3D reset history. Camera history invalidates without camera constants. | Use the same deterministic boundaries. No explicit camera-cut detector was found here; abrupt cuts need a reset signal and regression. |
| Reactive mask | Optional R8 mask derived from scene changes/opaque snapshots. | MetalFX supports this input, but its response differs. Apple cautions against reusing a mask tuned for another upscaler unchanged. Retune/validate transparency, particles and fog. |
| Output/alpha/UI | RGBA16F output; HDR merge preserves game alpha; UI follows temporal reconstruction. | Preserve that composition contract. Do not put HUD into history or assume MetalFX alpha carries the game's post data. This prototype checks RGB only. |
| Sharpening | Optional RCAS, default strength 0.3, plus possible extra sharpening. | MetalFX is a separate reconstruction algorithm, not bit-exact FSR/RCAS. Evaluate quality independently and budget any extra sharpening pass. |

Apple documents [motionVectorScaleX](https://developer.apple.com/documentation/metalfx/mtlfxtemporalscalerbase/motionvectorscalex),
[motionVectorScaleY](https://developer.apple.com/documentation/metalfx/mtlfxtemporalscalerbase/motionvectorscaley),
[depth convention](https://developer.apple.com/documentation/metalfx/mtlfxtemporalscalerbase/isdepthreversed),
[jitter](https://developer.apple.com/documentation/metalfx/mtlfxtemporalscalerbase/jitteroffsetx),
[pre-exposure](https://developer.apple.com/documentation/metalfx/mtlfxtemporalscalerbase/preexposure)
and [exposure texture](https://developer.apple.com/documentation/metalfx/mtlfxtemporalscalerbase/exposuretexture).
Its [WWDC25 guidance](https://developer.apple.com/videos/play/wwdc2025/211/) places
linear-color upscaling before post/tonemapping and explains exposure and
reactive-mask qualification.

## Synthetic timings

All runs use 2560 × 1440 inputs and 3840 × 2160 RGBA16F output, constant color,
constant depth and zero motion. Temporal history remains active during
measurement. Twelve dispatches warm up, followed by 60 samples with a host
completion wait between dispatches.

MetalFX time is the Metal command buffer's GPU start/end interval; that buffer
contains only the encode. FSR time uses Vulkan timestamps bracketing the existing
portable FSR 3.1 context dispatch on the same device/images. FSR flags match the
HDR prototype, not the production scaled-LDR branch. Input rendering,
presentation, readback, creation and CPU encoding are excluded.

Latest combined D32F/S8 + presentation run:

| Effect | Mean GPU ms | Median ms | p95 ms | Min/max ms |
| --- | ---: | ---: | ---: | ---: |
| MetalFX temporal | 1.599 | 1.691 | 2.008 | 1.089 / 2.067 |
| MetalFX spatial HDR | 1.768 | 1.536 | 2.783 | 0.938 / 5.925 |
| FSR 3.1 without RCAS | 6.526 | 6.454 | 10.701 | 3.658 / 11.743 |
| FSR 3.1 with RCAS 0.3 | 16.756 | 17.210 | 26.649 | 8.143 / 27.012 |

**Provisional, contention-sensitive measurements.** Other agents' GPU workloads
were not stopped. Earlier temporal MetalFX means were 2.10–3.10 ms and FSR
without RCAS means 7.80–16.47 ms. Variation prevents a reliable speedup ratio or
incremental RCAS cost estimate. Flat input does not establish real-scene quality
or cost. Rerun in an agreed quiet window, then measure representative captured
frames before making a 16.67 ms frame-budget decision.

This establishes API/execution feasibility and promising temporal cost, **not
4K60 gameplay**, image-quality acceptance or production readiness. Future
integration must fail clearly on missing capabilities/formats,
export/synchronization failures and correctness failures. Select MetalFX
deliberately as a provider; never quietly downgrade to spatial scaling or FSR.
