# Buffer barrier tracking (Apple)

`Runtime` (vk_runtime.cpp) records every buffer access since the last emitted barrier in a
tracker (`AccessBuffer`) and checks new commands against it (`IsBufferAccessed`): a command that
reads what was written, or writes what was read or written, flushes a barrier first. Two
corrections on Apple make the tracker see every dependency it should.

## Guest memory is keyed by guest address

Guest memory is reached through imported arenas (`BufferCache::GetArena`). The tracker used to
key an access by `Buffer*`, so an access through one import and a check through another import
of the same guest bytes missed each other. `TrackedRange` now keys guest-backed buffers
(`cpu_addr != 0`) by guest address in one resource space, for `AccessBuffer`, `IsBufferAccessed`
and the access memo alike; empty ranges access nothing, and a range whose end overflows stops.
This detects the dependency. It does not make two imports of the same bytes coherent within a
submission (the GPU does not): the buffer cache registers non-overlapping arenas and completes
the recorded work before a replacement import is used (`HostImportArenaMap::Obtain`).
Test: `barrier-alias-test`.

## Fixed-function input reads are recorded after the draw

Draws and dispatches read some buffers outside their descriptors: the index range, indirect
draw arguments and count, indirect dispatch arguments, and (geometry-stage emulation) the guest
index buffer the ES kernel loads. These used to be checked against pending writes but never
recorded, so a later transfer overwriting them (a DMA copy or fill, an HLE copy) got no barrier
after the command that read them (write after read). `Rasterizer::InputRead` notes them when they
are resolved, with their stages (index input, draw indirect, compute for the ES), and
`ResetBindings` records them after the draw or dispatch. Recording them at resolution instead
would be wrong: a `FlushBarriers` between resolution and the draw clears the tracker. The
tracker's read mask includes indirect command reads. The ES kernel loads whole dwords, so its
range is `GsIndexLoadRange` (the halfword before a range that starts mid-dword and after one that
ends mid-dword included).

**Vertex data needs no entry on Apple.** Vertex fetch is emulated (`profile.emulate_vertex_fetch`,
vk_pipeline_cache.cpp): the fetch shader is translated into the vertex shader, its vertex
buffers are ordinary `stage.buffers` entries, bound as descriptors, and `ResetBindings` records
them (`bound_buffers`) like every descriptor buffer. `ResolveVertexBuffers` returns before
binding any host vertex buffer.

`BB_INPUT_READ_VERIFY=1` checks that the bound index range of ordinary (not geometry-stage
emulated) indexed draws, and the arguments (and count) of ordinary indirect draws and indirect
dispatches, are among the noted reads, and stops naming the binding otherwise. Outside its
coverage: geometry-stage emulated draws (their ES index reads are recorded but not checked
here) and the recording of descriptor buffers.

## Other platforms (for upstream)

These changes are Apple-only: Linux and Windows cannot be built or run here. Upstream shadPS4 has
the same input-read gap: `ResolveIndexBuffer`, the indirect arguments and count in
`DrawIndirectRecord`, and the arguments in `DispatchIndirectRecord` are only checked with
`IsBufferAccessed`, never recorded with `AccessBuffer`, and the tracker's read mask lacks
`eIndirectCommandRead`. Platforms that bind host vertex buffers (no vertex fetch emulation) also
need their vertex ranges (`ResolveVertexBuffers`) recorded, with the vertex attribute input
stage.
