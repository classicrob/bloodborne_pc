# Guest format support on macOS

At device creation `Instance::CollectImageFormatInfo` logs every guest surface or depth format whose
Vulkan format lacks a feature the renderer might ask for. These lines are a capability inventory,
not evidence that the game uses the format. Correctness is decided where a format is actually used,
so on Apple every use that the device cannot honour is a hard error:

- `Image::Image` (`texture_cache/image.cpp`): image creation that the device reports as
  `eErrorFormatNotSupported` aborts with the format, size and guest address. Elsewhere it is
  logged and the image is created anyway, which is undefined behaviour in MoltenVK.
- `ImageView::ImageView` (`image_view.cpp`): creating an image drops storage usage when a format
  refuses it. A later storage view of such an image aborts.
- `GraphicsPipeline` (`vk_graphics_pipeline.cpp`): a color target whose format cannot be a color
  attachment aborts. Elsewhere it is a warning.

## The eleven startup lines on M4 Max (MoltenVK 1.4.2), first production run

| Guest format | Vulkan format | Missing on M4 | Verdict |
|---|---|---|---|
| 2_10_10_10 UINT | `A2B10G10R10UintPack32` | VertexBuffer | Harmless: Apple pulls vertices in the shader, so no fixed-function vertex input uses it. |
| 2_10_10_10 SNORM | `A2B10G10R10SnormPack32` | sampled, storage, color, transfer | Metal has no 10:10:10:2 SNORM. A gap only if used as an image. That use aborts at image creation. |
| 2_10_10_10 SINT | `A2B10G10R10SintPack32` | everything | As above. |
| 4_4_4_4 UNORM | `B4G4R4A4UnormPack16` | storage, color attachment | Sampling works. Rendering to it or storing to it aborts (pipeline or view check). |
| 5_6_5, 1_5_5_5, 5_5_5_1 UNORM | `R5G6B5`, `A1R5G5B5`, `R5G5B5A1` | storage | Sampling and rendering work. A storage view aborts. |
| 32_32_32 FLOAT / UINT / SINT | `R32G32B32*` | sampled image, transfer | Metal has no 96-bit texel format. Buffer reads of this format are recompiler loads and are unaffected. As an image it aborts at creation. |
| Z16 + S8 | `D16UnormS8Uint` | depth-stencil attachment | **Real semantic gap, substituted silently.** See below. |

Nothing in this table was seen being used by the game. The checks above will name the first use.

## D16 + S8: open decision

`Instance::GetSupportedFormat` replaces `D16UnormS8Uint` with `D24UnormS8Uint` or, on Apple GPUs
(which have no D24S8), `D32SfloatS8Uint`. That substitution is not exact:

- Depth is stored with float precision instead of 16-bit UNORM, so depth-test ties and
  near-equal comparisons can resolve differently from the PS4. Sampling the depth also returns
  values not quantized to 1/65535.
- The constant depth bias is scaled by the format's minimum resolvable difference: 2^-16 for D16
  UNORM, but 2^(e-23) per primitive for D32 float. The same guest bias registers therefore produce a
  much smaller offset, which can cause shadow acne or z-fighting where the PS4 had none.
- Metal can attach `Depth16Unorm` and `Stencil8` as separate textures, but Vulkan dynamic rendering
  requires the depth and stencil attachments to be the same image view. So with MoltenVK there is
  no exact combined D16 + S8 attachment.

Decision (orchestrator, 2026-10-10): hard error on Apple now, naming the guest image
(`Image::Image`) or, for a pipeline depth attachment, the format (`Instance::GetSupportedFormat`),
so the first use tells us whether Bloodborne needs Z16 + S8. If it does, the follow-up is designed
for exactness first: quantize depth to 16-bit in the FS (`round(z * 65535) / 65535`) with exact
bias rescaling, and measure the early-Z cost. Only then is it compared with a documented
bias-scaling approximation, which needs explicit sign-off as an accepted deviation. The `R8Srgb`
and `R8G8Srgb` substitutes are refused on Apple too.

`R8Srgb` and `R8G8Srgb` have UNORM substitutes in the same function, but M4 supports both sRGB
formats (sampled, color, storage), so those substitutions never run on Apple. Measured by a direct
`vkGetPhysicalDeviceFormatProperties` probe (`out/rect-iface/fmt/formats-m4.txt` in the
Opus-Renderer worktree). On the same device `D16_UNORM_S8_UINT` and `D24_UNORM_S8_UINT` report no
optimal-tiling features at all, and `D32_SFLOAT_S8_UINT` and `D16_UNORM` are depth-stencil
attachments. So every guest Z16 + S8 image on Apple takes the D32F + S8 substitute.
