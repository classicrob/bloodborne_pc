# MetalFX feasibility on the Rosetta macOS port

Tested 2026-10-10 on M4 Max, macOS 27.2 (26B5101f), macOS 26.4 SDK,
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

### Code 26: signature-dependent creation error

The same x86_64 probe creates cleanly after ad-hoc signing, and logs Apple
Neural Engine Code 26 after removing that signature. Signed native ARM creation
is also clean. The repeatable matrix on this OS is:

| Configuration | Creation result | Creation time |
| --- | --- | ---: |
| Signed arm64 | Temporal and spatial nonnull, no ANE error | 1580 ms |
| Signed x86_64 under Rosetta | Temporal and spatial nonnull, no ANE error | 1716 ms |
| Same x86_64 binary, signature removed | Temporal nonnull; ANE model compilation Code 26 | 5405 ms |

This isolates a code-identity-dependent failure; it is not evidence that
Rosetta prevents MetalFX execution. The unsigned diagnostic is a deliberately
invalid configuration, not a supported provider path. The module uses
Security.framework's SecCodeCopySelf/SecCodeCheckValidity with strict validation
before creating the scaler and aborts if the host signature is missing/invalid.
All normal prototype and development outputs are signed and verified.

Neither the SDK 26.4 public MetalFX headers nor its descriptor expose an ANE
backend selector or execution-backend status. All three probes print the same
diagnostic scaler class, which does not prove the internal implementation.
We cannot establish the quality or performance of the unsigned error path and
do not ship it. There is no application-selected substitute effect.

The SDK documentation for
[requiresSynchronousInitialization](https://developer.apple.com/documentation/metalfx/mtlfxtemporalscalerdescriptor/requiressynchronousinitialization)
describes synchronous compilation as making the fastest compiled effect ready
at creation. Asynchronous initialization may use a slower interim effect while
compilation proceeds; the documented image quality is the same. That documented
initialization behavior is distinct from the unsigned Code 26 error. The module
always requests synchronous initialization.

Apple describes the APIs in its
[MetalFX overview](https://developer.apple.com/documentation/MetalFX).
Rosetta support above is observed, not inferred from that page.

## Reproduce

Use the existing shared x86_64 dependency prefix and already-built FSR libraries:

~~~sh
BB_DEPS_PREFIX="$HOME/bbport-deps/x86_64" bash tests/macos_metalfx/run.sh
BB_DEPS_PREFIX="$HOME/bbport-deps/x86_64"   bash tests/macos_metalfx/run.sh --depth-stencil --present
BB_DEPS_PREFIX="$HOME/bbport-deps/x86_64"   bash tests/macos_metalfx/run.sh --module --depth-stencil --present
bash tests/macos_metalfx/signature.sh
PATH=/opt/homebrew/bin:$PATH bash tests/macos_metalfx/jitter.sh
BB_DEPS_PREFIX="$HOME/bbport-deps/x86_64" bash tests/macos_metalfx/run.sh --module --reactive-history
BB_DEPS_PREFIX="$HOME/bbport-deps/x86_64" bash tests/macos_metalfx/run.sh --ldr-padded
~~~

The optional --present briefly creates a 640 × 360 SDL/Metal window. Scaling
remains 2560 × 1440 to 3840 × 2160; the presentation blit fits that output to the
window. This does not test a 4K physical display or the MacBook panel.
BB_FSR_STATIC_DIR can select the existing shared build's
out/gpu/third_party/fsr-vulkan. No second GPU tree is built.
Missing libraries, unsupported features, failed commands and incorrect pixels
exit nonzero. Tests run sequentially without debugger/sampling tools.

support.mm is the smaller descriptor-creation probe. signature.sh runs the
signed ARM/signed Rosetta/unsigned Rosetta creation matrix sequentially; its
unsigned copy is solely the expected-error diagnostic. interop.mm is the actual
encode/interop/presentation/benchmark program. jitter.sh generates a moving
analytic grid with nonzero X/Y jitter through Vulkan and compares four sign
combinations against its unjittered reference. Scratch is approximately 12 MiB
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
| Jitter | Centered Halton(2,3), ±0.5 render pixels, scale-dependent phase count. FSR defaults to positive X-right/Y-down viewport-shift components. | Pass the same X/Y viewport shifts in render pixels. The moving-grid test gives mean absolute error 0.003398 for (+,+), versus 0.005303 (+,-), 0.005662 (-,+), and 0.007796 (-,-). It uses previous-current motion (-0.125,-0.0625) with persistent history. This qualifies the convention, not complete disocclusion/game quality. |
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

## Standalone Apple module

`gpu/shim/macos_metalfx.h/.mm` is independent of the renderer and is not linked
into libbbgpu or exposed as a setting yet. Its C++ header hides Objective-C
objects. One owner thread creates/encodes/drains the provider:

- Config fixes input texture/output extents, D32F versus D32F/S8, an in-flight
  limit, the color format (RGBA16F linear HDR, or tonemapped RGBA8 UNORM/sRGB),
  the output format (RGBA16F or RGBA8 UNORM), auto exposure (HDR only; an RGBA8
  input with auto exposure aborts) and the content extent: the frame may be the
  top-left of larger aligned input textures (inputContentWidth/Height). The
  scale factor is output / content. Values are used as stored; no inverse gamma.
- Register exports and validates the five Vulkan images once per resource
  generation: color and output in their configured formats, RG16F motion,
  depth, and R8 reactive mask; all inputs at the input texture size.
  Cached bindings retain their Metal textures. A unique retained owner token
  rejects bindings from another provider, including after address reuse.
- Submit waits for a producer timeline value, encodes temporal scaling, signals
  a consumer value, and retains the binding until Metal completion. It requires
  increasing event values/frame IDs and positive finite pre-exposure. First
  frame, a gap in frame IDs, or a changed history epoch resets history.
- Drain waits only at resize/destruction/tests. The ordinary encode does not
  wait on the CPU. Command failures and exhausted in-flight capacity abort with
  an explanation; they do not choose another upscaler.

The caller owns Vulkan resources and must retire both APIs before deleting
them. Metal texture retention is not permission to destroy an in-use VkImage.
LastGpuMilliseconds reports the completed command-buffer interval, potentially
including an event wait; it is not necessarily isolated reconstruction time.
The synthetic benchmark below uses separate encode-only command buffers.

The module has passed cached-export alternating-color/reset tests, D32F/S8,
moving-grid history/jitter checks and module-output Vulkan presentation. These
are synthetic tests, not HDR integration, scene-quality or game acceptance.
The reactive-history test leaves the epoch unchanged while alternating the
input color and setting mask=1; checked output follows the current frame.
The LDR-padded test (`--ldr-padded`) puts a 2560x1440 RGBA8 frame in 2624x1472
textures with green padding and upscales it without auto exposure to RGBA16F
and to RGBA8 UNORM output; the center and bottom-right output pixels must show
the frame only (they show the padding when the content extent is ignored).

## Renderer integration proposal

The following touchpoints were sent to Sol-3 for ownership review. No changes
have been made in these renderer files. The provider must remain unavailable
until every contract below is implemented and tested; selecting an unavailable
MetalFX configuration must fail with its specific missing requirement.

### Provider and HDR placement

Append an Apple-only MetalFX choice beside FSR 3.1 and TAA in bbport_settings,
launcher/overlay selection and persisted names. Preserve existing enum values
and non-Apple choices. Selecting MetalFX creates exactly this temporal provider;
unsupported devices/formats/signatures must abort, rather than select FSR or
spatial scaling. Add an Objective-C++ source target and Foundation, Metal,
MetalFX, Security frameworks only on Apple in gpu/CMakeLists.txt.

TemporalUpscaler::OnDispatch currently rejects Scaled() and uses the HDR
post-combine shader hook only for native AA. The MetalFX branch must use that
HDR hook for native AA and scaled presets, before exposure/tonemapping, instead
of inserting into RunScaled's current RGBA8 tonemapped path. Do not infer the
game's transfer function from RGBA8 format or apply a guessed inverse gamma.

SceneTargets presently allocates one reduced-resolution proxy tier. MetalFX
requires a second output-resolution tier for post passes after the HDR hook:
redirect their scene-color reads and post destinations to the reconstructed
output while earlier raster passes retain render-resolution proxies. Image
views, shader coordinate constants and aligned extents must follow that tier.
EnsureResources must create exact active extents for the current module; it
rejects padded extents rather than processing uninitialized padding. Supporting
padded textures would need an explicit active-region API/test first.

This placement increases post-processing bandwidth/work from render pixels to
output pixels. At 2560×1440 to 3840×2160 that is 2.25 times the post pixel count.
Measure this cost against the existing late-LDR FSR path before recommending
MetalFX for 4K60. One additional RGBA16F 4K output is about 63.3 MiB; MetalFX's
internal allocations require a separate measurement.

### Scheduler, export and resource lifetime

Declare VK_EXT_metal_objects export intent in instance/device setup, image
creation and the two timeline-semaphore creations. Keep a provider and cached
binding per resource generation, not per draw/frame. Export failures are hard
errors. Do not add a copy-based interoperability path.

At the HDR hook, end active rendering and release color, depth, motion,
reactive and output ownership to VK_QUEUE_FAMILY_EXTERNAL in GENERAL layout.
Flush the Vulkan producer with ready=N. Submit MetalFX waiting on ready=N and
signaling complete=N. The following Vulkan submission waits on complete=N and
acquires all those resources back before post processing or reuse. Update the
renderer layout/ownership records together with these barriers.

Scheduler::SubmitInfo has four wait/signal slots. A pending external wait must
survive the normal next-submit/Flush path without dropping existing waits or
overflowing that capacity. Do not use Scheduler::Finish or a host event wait
between these phases. Resize/provider replacement drains Metal and waits the
final Vulkan consumer retirement tick before destroying images/events/scalers.
Defer destruction on the provider's encoding thread. Shutdown must drain both
APIs; a retained Metal image alone does not establish Vulkan retirement.

The added hot-path work is per-frame: one submission boundary, two shared-event
operations, ownership bookkeeping and one Metal command buffer. Texture exports,
signature validation and scaler compilation are configuration-time work. There
is no per-draw export or inter-API pixel copy. Scheduler CPU overhead and total
latency still need measurement.

### Exposure, reactive mask, cuts and composition

| Contract | Chosen behavior and acceptance requirement |
| --- | --- |
| Motion/depth/jitter | Previous-current render-pixel motion at scale 1, normal device depth and the same positive viewport jitter, as qualified above. Test camera and object motion, disocclusion and depth edges on captured frames. Do not include a second jitter delta in motion. |
| Exposure | Linear HDR, auto exposure enabled, preExposure=1 while game HDR is not pre-exposed. Verify the game's tonemapper brightness against MetalFX's exposure diagnostic. If its pre-exposure changes, supply the actual positive multiplier; do not silently retain 1. Exact game exposure behavior is an integration acceptance gate. |
| Reactive mask | Always bind an initialized R8 mask. Zero means ordinary reconstruction; one excludes history. Create a MetalFX-specific material/transparency mask, not the current AMD-tuned heuristic unchanged. Qualify particles/fog, mask range and current-only pixels. A deliberately disabled mask is an explicit zero-mask setting, never recovery for a failed generator. |
| Camera cuts | Pass one history epoch from OnFrameStart/CameraMotion. Increment on output/preset/provider change, missing frame/camera history, UI/loading-to-3D transition and a verified camera-cut signal. The current code lacks an explicit cut detector: trace camera/guest transition metadata and test abrupt cuts before enabling. Do not call a guessed threshold exact or silently retain stale history. |
| Alpha | MetalFX reconstructs RGB; it is not the source of game post alpha. Preserve the existing native-AA RGB-only merge contract. For the new output tier, audit each guest post consumer's alpha meaning and coordinate mapping, then carry/resample that auxiliary data with the same documented sampler semantics. Test auxiliary alpha bit/data preservation; guessing an alpha interpolation mode is not accepted. |
| UI | Game UI renders after tonemapping at output resolution, outside jitter/history. A menu-only frame has no temporal scene; its explicitly identified UI/background composition follows the existing UI path. Loss of a required 3D input is an error, not a temporal-to-spatial switch. |
| Sharpening | MetalFX is its own reconstruction algorithm. Any extra sharpening is a separate selected post pass with measured cost and image-quality tests; do not reinterpret RCAS strength as a MetalFX parameter. |

Apple's [WWDC25 integration guidance](https://developer.apple.com/videos/play/wwdc2025/211/)
places linear-color reconstruction before post effects and warns against
reusing another upscaler's reactive-mask tuning. The placement above follows
that guidance; game-specific exposure, cuts and auxiliary alpha remain required
qualification work, not facts established by constant-color tests.

The output goal is 60 FPS with temporal reconstruction: 2560×1440 rendering to
3840×2160 on the external display, or 2304×1296 to a 3456×1944 16:9 image on the
3456×2234 panel, with 145-pixel top/bottom bars. Native 4K rendering is not
required. Qualify resize, preset changes, cuts, UI, particles and simultaneous
Vulkan/Metal resource retirement at both outputs, then benchmark representative
gameplay. A synthetic encode time does not establish a 16.67 ms game frame.

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
| MetalFX temporal | 1.737 | 1.412 | 3.236 | 1.187 / 3.796 |
| MetalFX spatial HDR | 1.560 | 1.448 | 2.764 | 0.907 / 3.202 |
| FSR 3.1 without RCAS | 19.024 | 18.461 | 26.130 | 11.610 / 26.763 |
| FSR 3.1 with RCAS 0.3 | 18.748 | 19.068 | 27.530 | 7.769 / 28.239 |

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
