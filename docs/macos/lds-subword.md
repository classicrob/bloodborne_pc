# Concurrent halfword LDS stores

On devices without Workgroup explicit memory layout, `SharedMemorySimplifyPass` lowers
16-bit LDS accesses to a U32 shared array. Its former store sequence loaded a dword,
inserted one halfword, then stored the dword. Two lanes writing adjacent halves could
both read the old dword and overwrite each other's updates.

The lowered store now carries a shifted value and mask in `WriteSharedMaskedU32`.
The emitter atomically loads the dword and attempts a compare-exchange of
`(old & ~mask) | (value & mask)`, retrying if another writer changed it. Each successful
write replaces one complete halfword and preserves the neighboring halfword. Competing
writes to the same halfword resolve to one writer's complete value, rather than a mixture
of bits from multiple writers. Devices supporting direct Workgroup U16 accesses retain
that path.

The same masked store is retained when LDS is spilled into a storage buffer.
`SharedMemoryToStoragePass` converts the workgroup-relative byte address to the typed
buffer element index after adding the workgroup's byte-sized allocation offset. This
also corrects the previous byte-versus-element mismatch for existing spilled accesses.
Barriers for spilled LDS also include `UniformMemory`, so the existing workgroup barrier
synchronizes the storage-buffer accesses as well as Workgroup memory.
GDS halfword stores are converted earlier to native storage-buffer U16 stores and retain
that path.

The CAS path costs one atomic load and one compare-exchange per attempt, with retries
only on a competing update. It adds no work to ordinary U32/U64 LDS accesses. The
standalone test is a correctness stress test, not a performance or game-FPS benchmark.

Run against a stable shared production library:

```sh
BB_GPU_BUILD=/absolute/path/to/out/gpu bash tests/test_lds_subword.sh
```

The test compiles the actual production shared and buffer emitters, runs 64 dispatches
under Rosetta/MoltenVK, checks 4096 lane results per dispatch, and validates four SPIR-V
modules. It covers adjacent halves and 64 lanes competing for one halfword, both in
Workgroup memory and a storage buffer, with untouched-neighbor and canary checks.
The independent GCN conformance suite's `lds` group exercises translation and the
simplification/spill passes using encoded DS instructions.
