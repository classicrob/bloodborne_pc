# Character preview: capture and memory-side triage

The October 10 capture establishes two connected offscreen render chains, followed
by UI sampling of the first chain's final image. It does **not** establish where
pixels become blank. No renderer or memory fix follows from this evidence alone.

## Provenance

Opus-Playtest captured `frame_1791673169.txt` and `frame_1791673176.txt` in
`out/macos_route/20261010-185640-1080p-DIAG-preview-capture-bit42-9e79075/`
in the playtest worktree. Both screenshots show an empty character panel and
otherwise populated Contract UI. The pinned production artifacts are from
`9e79075`: probe SHA starts `53f1ff89`, GPU library SHA starts `377faeac`.
The run is a diagnostic with toggle bit 42 disabled, 1080p output, FSR 3.1 native
AA, and cold caches. Neither LDS16 nor wave64 repairs are in that build.

The graph, relevant target identities, and geometry draw/index counts agree in
both captures. Shader names below are the logged low 32 bits of their hashes.
Addresses are observations from this run, not constants for a later launch.

## First preview chain

| Pass | Operation | Relevant output or input |
| --- | --- | --- |
| 1 | Depth-only triangle | D32S8 `0x7072240000`, 784 x 784 |
| 2 | Seven dispatches of `8b355b5a` | Packed-color buffer clears, including HDR/image memory |
| 3 | Geometry, 35 draws / 154,614 indices | G-buffer color 0 `0x7073680000`; depth `0x7072240000` |
| 8–9 | Deferred lighting | Samples G-buffers and R32 depth-copy `0x70726b0000` |
| 11–15 | Color/filter/composite passes | Final HDR `0x7072b20000`, RGBA16F |
| 19 | Tonemap, fragment shader `4f4afe5c` | Samples HDR; outputs RGBA8 `0x7071f60000` |
| 40 | UI, 31 draws / 1,764 indices | Samples `0x7071f60000`; outputs `0x704ebf8000` |

All preview targets above are 784 x 784; the UI output is 1920 x 1080.
Pass 22 renders a second geometry chain with 29 draws / 79,017 indices, G-buffer
color 0 at `0x7077b40000`, and depth at `0x7076700000`. Its HDR is
`0x7076fe0000`, and pass 38 writes RGBA8 `0x7076420000`.

The captured geometry shader list includes vertex shaders `a7450683`, `ddbe557e`,
and `3deaa573`. Inspected vertex IR performs skinning using buffer matrix rows;
`8b355b5a` is a packed-color fill, not the skinning kernel. Some captured position
and matrix prefixes are plausible finite values. This does not prove the complete
vertex transform, clipping, depth state, or resulting coverage is correct.

## Buffer-to-image coherence audit

These source paths are identical between the inspected checkout and `9e79075`:

1. On Apple, `BufferCache::ObtainBuffer` obtains a host import over the guest Mach
   backing. Its writable mirror is a shared mapping of those bytes.
2. Texture cache `Image` uses `UniqueImage::Create` and VMA image allocation.
   It is a separate Vulkan image, not another host import of the guest bytes.
3. `Rasterizer::BindBuffers` calls `InvalidateMemoryFromGPU` for every written
   buffer. Images with the same guest base receive `GpuDirty`.
4. `FindRenderTarget` and `FindTexture` refresh dirty images through
   `UpdateImage` / `RefreshImage`. `GpuDirty` bypasses the unchanged-CPU-hash
   suppression used to protect GPU-modified images.
5. `ObtainBufferForImage` returns the imported source on Apple. Tiled images go
   through `DetileImage`, which flushes outstanding memory/image barriers before
   dispatch. Linear uploads check tracked source writes in `UploadImage`.
   `ResetBindings` records compute shader writes; `FlushBarriers` publishes those
   writes to all-command memory reads/writes before the transfer/compute consumer.

Thus the exact-base clears of HDR `0x7072b20000` and intermediate
`0x707c320000` have an explicit synchronization/upload path. No missing step is
proven statically. The captured compute dispatches were not consumed by the
`IsComputeImageClear` shortcut: dispatch capture occurs only after resource binding
returns success. An independent caveat is that GPU invalidation ignores images
whose base differs from the written buffer's base; these captures do not establish
such a partial-overlap case as the cause.

## What the capture cannot prove

`FrameCapture::Buffer` copies a small CPU-side prefix before GPU execution.
Packed pixel or vertex-attribute words printed as floating-point NaNs are not
proof of NaN shader inputs or compute outputs. The graph limits stored shaders,
textures and buffer samples, and does not record complete raster/depth state.
Pass membership of early binding notes can precede `BeginPass`.

The current `BB_FRAME_DUMP_TRIGGER` only dumps the guest display image and
presented output from `vk_presenter.cpp`. It cannot select the offscreen targets
above. A synchronized sampled-image readback is needed to compare G-buffer,
depth-copy, HDR and final preview as their consumers see them. A connected graph
alone does not prove UI blend/alpha or sampling correctness.

## Next discriminator

Recheck the preview on the post-quiet-window build containing both LDS16 and
wave64 before investigating the older failure further. If it remains blank,
Opus-Renderer owns the opt-in sampled-image dump tool. Capture G-buffer color 0,
R32 depth-copy, HDR, and final preview at their sampling points. Valid G-buffer
coverage followed by empty HDR localizes the problem differently from an empty
G-buffer/depth pair; a populated final image requires examining UI sampling and
blending instead. No new game or GPU test was launched for this audit.
