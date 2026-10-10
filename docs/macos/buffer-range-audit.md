# Buffer binding range audit

The black-frame investigation on 2026-10-10 found no Apple-specific truncation in
the runtime mapping queries. This is a source audit plus an actual Mach-backend
regression on macOS 27.2 under Rosetta, not a Linux execution comparison or proof
that every live draw has the correct descriptor.

## Query and import contract

`gpu/shim/bbgpu.cpp` routes `MemoryManager::ClampRangeSize` directly to
`runtime_memory_clamp` in `src/runtime_memory.c`, on every platform. The clamp
queries the runtime's guest VMA table, not Mach's host VM region table. Starting
inside a mapped VMA, it walks adjacent non-reserved VMAs until the requested size,
a virtual gap, or a reservation. Protection, mapping type, physical offset and
physical alias identity do not stop this walk. Even a guest PROT_NONE mapped VMA
is considered backed; host GPU write tracking also does not change this extent.

The per-thread region cache can return the requested size early when it fits
inside a cached mapped VMA. A request crossing that VMA's end takes the full table
walk; it does not return the cached VMA's remaining bytes. Writer generations
invalidate cached extents after map, unmap, replacement, protection and release.

Apple-only `GetMappedRegion` and `GetBackingRegion` return a single guest VMA.
`BufferCache::GetArena` walks those regions and expands an import to encompass the
request and its containing VMAs. It asserts the whole import is mapped and obtains
backing identity for every segment. These region ends do not directly set the
Vulkan descriptor range. The ordinary `BindBuffers` path asserts on Apple that
the clamped size equals the guest descriptor's size, then binds `size + adjust`
bytes at the aligned offset, with `adjust` supplied in push data. An import may
be larger than that binding without reducing the binding.

## Important qualification: the constant ring

`PrepareStageConstants` also clamps small read-only guest buffers before copying
them into `constant_ring`. `BindBuffers` binds `ring->size` directly for these
buffers, bypassing the ordinary-path equality assertion. A genuinely unmapped
portion could therefore produce a smaller ring descriptor without that assertion.
This path is shared with Linux. It remains a place to log original descriptor
size versus ring size if a failing shader uses a ring binding. No evidence of such
a truncation was found in this audit; the capture below does not include those
ring buffer headers.

Likewise, runtime low allocations and loader mappings are outside the guest VMA
table, so this clamp reports zero for them on all platforms. A GPU descriptor
pointing there would need separate investigation. The sampled buffers below are
ordinary direct-memory addresses.

## Executed regression

Run `sh tests/test_memory_clamp.sh`. The signed SDK-only x86_64 executable includes
the actual runtime backend and performs real mappings. On macOS 27.2 it passed
15,120 cached/uncached clamp checks plus region and full-range mapped queries:

- Interior constant/vertex-sized requests, including 864-byte and unaligned bases.
- Guest protection splits, including a backed PROT_NONE section.
- A 4-KiB GPU protection split within a 16-KiB guest page.
- Adjacent direct VMAs with noncontiguous physical offsets.
- Direct/flexible/direct boundaries and adjacent physical aliases.
- Warm-cache invalidation after unmap, reserved-hole replacement and physical release.

The test also has a Linux compile/run path, which has not been executed here.
No production memory or shader recompiler code changed.

## Live experiment evidence

Astra-1's experiment `play-bounds-1080p-20261010-172825.log` records a direct
mapping `[0x7042c00000, 0x70d1b00000)`. All three buffer headers in the supplied
`captures/frame_1791667455.txt` fit wholly inside that interval:

| Shader | Slot | Guest base | Captured bytes |
| --- | --- | --- | --- |
| `8b355b5a` | 1 | `0x704ebf8000` | 8,355,840 |
| `34e8a281` | 1 | `0x704d37d030` | 262,144 |
| `34e8a281` | 2 | `0x704d37d040` | 262,144 |

The log has no clamped-size or incomplete-mapping diagnostic. These are a supplied
capture and a separate experiment log with the same guest mapping addresses, not
a synchronized dump of the live VMA table; intervening unmaps are not excluded by
that comparison alone. Astra-1 also reports black output with either dense or
imported arenas when real bounds checks are on, and a colorful title with the dense
arena when bounds checks are off. That experiment further argues against an
import-only range difference; it does not establish which bounds input is wrong.
