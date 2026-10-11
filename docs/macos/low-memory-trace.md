# Low host-memory provenance

`BB_LOW_MAP_TRACE=1` enables an Apple-only diagnostic. Each successful low map
prints its range, protection at creation, owner label and caller image/offset.
Labels distinguish image, main-stack, guest-stack, hle-stack, tls and host-heap.
The allocator additionally logs host-object alloc/free ranges and caller sites.
All emitted lines have a process-local sequence number; allocator ownership
changes are logged while holding its allocator lock. Output from separate
processes must not be combined without preserving process provenance.

Example run configuration:

```sh
BB_LOW_MAP_TRACE=1 bash run.sh --dump-shaders
```

Correlate the renderer's failing address with the containing map line, then the
latest alloc/free line covering that byte if it is a host-heap chunk. A free
line ends suballocation ownership, not the VM mapping. Coalescing can change a
later allocation's bounds. A stack map label establishes the kind of mapping,
not which callback is currently executing there or whether a local variable is
still alive. HLE stacks are pooled; this diagnostic does not log pool checkout.
Caller offsets can be symbolized offline against the exact signed binary.

This answers a gap exposed by the movie bind at `0x843400050+0x20`: the address
is inside the 32–63 GiB low arena, not the 448 GiB+ direct/flexible arena. Current
GPU region/backing queries do not track low mappings, so failure of those
queries does not establish that low memory is unmapped. Low maps advance
monotonically and the small allocator reuses mapped storage without unmapping.
The trace changes no mapping or import behavior and adds no host export.

AvPlayer frame bytes currently come from the game's allocate/allocate_texture
callbacks. The low heap directly owns the AvPlayer handle and callback bounce
buffers; thread stacks and TLS also use low maps. Ordinary guest malloc/new
are not overridden with host malloc by `runtime.c`. Owner evidence from an
actual failing run is required before assigning the bind to any of these.

The Linux buffer-cache branch stream-copies small read-only source ranges;
Apple's import branch currently requires tracked backing before reaching that
path. A valid low-memory bind therefore needs an explicit supported memory
contract, not a skipped bind. This diagnostic does not choose that contract.

Cost: off by default; cached pthread_once check on low allocation/free, no
per-draw work. Enabled tracing resolves a caller with dladdr and writes stderr
per map/alloc/free, which changes timing. Use for correctness, not performance.

`tests/test_low_map_trace_macos.sh` checks disabled output, map labels/ranges,
caller fields, suballocation free/reuse and preserved errno with the real
memory backend under signed x86_64 Rosetta execution.
