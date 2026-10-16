# Narrow BC4/integer cache alias repair (review candidate)

This route handles a cached BC4Unorm image and an R32G32Uint block-grid image at
one identical guest address. It does not establish that the historical live stop
used this effective descriptor. A storage operation on an effective BC4 descriptor
remains unsupported and loud.

Admission requires one complete mip, layer and sample, Color2D, identical byte
coverage without padding, exact 4:1 dimensions, identical linear or tile13 policy,
no metadata or other registered overlap, and no committed guest physical aliases.
Tile13 requires complete 8x8 microtiles of blocks. Partial views, chains, other
formats/layouts and simultaneous differently represented bindings stop before
conversion. The conservative whole-VM alias check can reject unrelated aliases.
Raw GPU buffer writes overlapping any already converted image are rejected at
the central written-buffer acquisition, including interior partial writes and
GPU fill/copy HLE destinations. This candidate does not merge simultaneous raw
buffer and image authority. Ordinary image invalidation policy is unchanged.
Existing renderer/VM exclusion protects the current mapping through command work;
a conversion-time inventory is not a lifetime pin by itself.

An ordinary CPU/buffer-dirty source with no image authority refreshes before the
full image copy. Mixed image and backing dirtiness is rejected. Converted images
are registered separately and old image/view deletion uses the existing deferred
retirement machinery. Registry generation invalidates lookup memos. Converted
images can only be used as complete sampled or integer storage resources.

Potential storage writes set image authority even when a shader branch may not
execute. A converted-image registry survives ResetBindings. Actual command-exit
hooks materialize any pending converted images: preflight every marked layout and
authority, copy/retile through TileManager into held staging, transfer/compute to
host barrier, Finish, invalidate, exact TryWriteBacking, then clear GPU authority.
No texture or VMA lock is held across tiling/recording or Finish: the tiling
parameter stream can itself wait on wrap. After joining the binding helper, the
Stage-B/VM mutation protocol keeps the selected active image alive outside the
texture lock; image UID/authority is checked again before publication. No pending
converted image means no submission or wait. This is a conservative correctness
path, with no speedup claim.

The unlocked image reference also relies on presenter ordering: CPU flips enter
through Liverpool SendCommand, whose command processing drains the draw pipe;
EOP flips run in the same ordered Stage-B task stream (or drained Stage A).
Presenter-thread DrawLastFrame uses PrepareLastFrame, which does not insert images.
The stream-wrap Flush callback flushes barriers and pending arena binds, rather
than calling OnSubmit/GC or PrepareFrame. Deferred deletions concern unregistered
slots, not this active registry member. New callers must preserve these exclusions.

The guarantee is publication before command recording completion and later ordered
labels/fences/flip; it does not promise visibility to unsynchronized CPU reads
racing Stage A enqueue. Before a VM mutation acquires its writer lock, renderer
work is drained and its helper joined, all marked authority is materialized, all
converted images are evicted, then Finish/PopPendingOperations retire the deferred
resources. This prevents later mapping aliases from inheriting stale image state.
No synchronous work is added to ResetBindings or submission/retirement callbacks.

Stage A classifies potential admitted integer-image writes from the selected
stage descriptors, including ES/GS stages. After committing the producer packet,
it records the producer-end cursor and byte ranges. Before the next draw/dispatch
pipeline selection, any outstanding candidate producer is drained, so SRT walkers,
fetch/tessellation reads and prepared-data revalidation see published backing.
No candidate or an already consumed producer adds no drain. The range notes also
cover parser-read overlap checks; ordinary buffer-write tracking is unchanged.

## Evidence and remaining validation

The existing one-mip/layer device primitive passed BC4->uint, immediate integer-byte
oracle, integer storage partial write, and uint->BC4 with all untouched blocks,
source and canaries preserved. Its three skip mutations failed corresponding
oracles. That proves the primitive on the recorded M4 Max/MoltenVK pair only.

The policy fixture checks both directions, linear/tile13 acceptance, nonzero mip
and layer view rejection, chains, sample count, padding, metadata, format, swizzle,
address and mixed-authority rejection. It runs inside the existing storage-alias
route target and can run without Vulkan under BC4_ALIAS_POLICY_ONLY. Synthetic
ImageInfo policy checks do not exercise RefreshImage or the VM/cache lifecycle.

Before integration acceptance, require independent descriptor and synchronization
review of the exact combined hash, renderer compilation and exact-tip full gate.
Still required: real cache conversion with CPU-dirty initialization, storage
partial write and sampled reverse ownership; byte-exact tile13 retile; real
same-command/helper alias rejection; future physical alias map/read/write and
unmap/remap at the same VA/size with different allocation generation; pending
resource retirement; no-alias/no-write command hook controls; legal label/CPU
readback publication. Tests must detect skipped writeback/retile/retirement as
appropriate. Live effective descriptor diagnosis and representative gameplay are
separate requirements. No repair runtime PASS is recorded yet.
