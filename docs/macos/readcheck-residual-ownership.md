# Ownership of the READBYTES residual buffers

Static audit, 2026-10-11. Image offsets use the 01.09 image and conventions in
[throttle-guest-lifetimes.md](throttle-guest-lifetimes.md). No new game run was
started. This is an ownership lead, not proof of a write racing GPU execution.

## What the capture establishes

Source: Playtest's
`out/macos_route/20261011-182450-1080p-READBYTES/game.log`, residual rows near
lines 2847–2870. All 17 non-tessellation changes belong to **frame 2069**, at
the cold-load transition, not 17 independent steady-state incidents over six
minutes. Renderer identifies the preceding frame as a roughly 16-second
stall with 68 shader/pipeline compiles. The checker timing fields across that
stall are under investigation and are not used here as ordering evidence.

| Rows | Address/mapping | Established scope |
| --- | --- | --- |
| 15 | `0x7113100fc4` through `0x7113103504`, within `[0x7113100000,0x7113600000)` | Dedicated 5 MiB guest direct-memory map, protection `0x33`, memory type 0, backing offset `0x10af00000`; VS constant-buffer ranges |
| 2 | `0x7056ce0ad0 + 4112` | The same range and draw 34504, observed at VS binding 6 and PS binding 0; inside the much larger direct map starting `0x7042c00000`, size 2398093312, memory type 3 |

The 15 ranges have sizes 32, 64, 96, 192, 288, 384 and 2304 bytes. Neither
mapping is the low host-allocation arena. Neither is in the previously
captured A1/A2 command pools (A2 ends at `0x7041400020`). Therefore their
addresses do not justify applying the command-chunk status protocol.

The read check compares recording-time bytes with bytes read **after** a
retirement observation. Legal reuse between retirement and the check also
changes this hash. A changed float proves different contents at the two
observations; it does not locate the write before the GPU's last read.

## Concrete candidate for the 5 MiB owner

The renderer has an explicitly configured 5 MiB suballocation heap:

1. Settings constructor `0x1d99260` stores `0x500000` at settings+`0x90`
   (`0x1d99351`). Main graphics setup `0x1d969f0` passes those settings to
   renderer creation `0x2194580` at `0x1d96bee`.
2. Creation copies settings to renderer R+`0xc0` at `0x2194727`. Thus the
   size is R+`0x150`, read at `0x21960b6`.
3. `0x21960f5` calls allocator `*[0x5540698]`, vtable+`0x50`, with that
   size. It stores the returned base at **R+`0x3e0`** and the allocator at
   R+`0x3d0`.
4. `0x219615a -> 0x2087e50` initializes a suballocation heap at R+`0x3f8`
   over that base and size. `0x21961a0 -> 0x207c000` registers the range
   against the allocator interface at R+`0x3e8`.
5. A preferred/fallback allocator wrapper is stored at R+`0x5e0` and
   global `0x55406b8` (`0x219620c/0x219621b`). Its preferred interface is
   R+`0x3e8`; its fallback is `*[0x5540698]`.

This matches the dedicated map's size and graphics use, but **the dynamic
identity is not yet proved**. The existing mapping log contains no guest
caller or owner pointer. Confirmation is the small read:
`R = *[image+0x55406c8]`, then `[R+0x3e0] == 0x7113100000`, with the heap
range at R+`0x498`/R+`0x4a0` covering that map. Address stability across
runs must not be assumed.

There are real graphics-resource clients of this wrapper. For example,
`0x217f76b/0x217f798` load `*[0x55406b8]` into R9 and pass it through
`0x2165c10` to resource initializer `0x216d420`. The latter allocates via
vtable+`0x58` at `0x216d4cb`, records allocator/data pointers at resource
`+0`/`+8`, and records size and flags. Some flags select another supplied
allocator, so this does not assign every graphics buffer to the 5 MiB heap.

## What the candidate allocator waits for

Relocations identify the preferred interface's vtable as `0x53b6770`:

| Method | Operation |
| --- | --- |
| `0x219cab0`, vtable+`0x50` | Lock the heap's CPU mutex, allocate with alignment 16 through `0x2088030`, unlock |
| `0x219cb10`, vtable+`0x58` | The same, with validated requested alignment of at least 16 |
| `0x219ccc0`, vtable+`0x70` | Lock, free through `0x2088900`, unlock |

The mutex is at owner R+`0x4b8`; none of these local paths polls a GPU label,
flip or completion event. The wrapper's free `0x21b34f0` finds the owning
allocator and dispatches to its free method. Resource destructor
`0x216d690` also frees its data through its stored allocator before removing
per-context resource indexes; see
[movie-resource-index-fault.md](movie-resource-index-fault.md).

These are ordinary heap operations, not a demonstrated per-frame rewind or
the A2 CAS/status protocol. Any GPU lifetime condition must be established
by the resource's caller. Moreover, the residual could be an in-place
update to a still-live resource: allocation/free tracing alone would miss
that. No writer PC or caller-level reuse predicate for these 15 rows has
been recovered from the existing capture.

For `0x7056ce0ad0`, only its containing large guest direct map and the two
bindings of one range are established. Assigning a suballocator or a
completion gate from the virtual address alone would be speculation.

## Smallest useful next evidence

First repair/qualify the check's retirement timestamps and obtain walking
coverage; do not add a lifetime workaround on the strength of these rows.
If the residual recurs, capture the renderer/heap pointers above and the
resource object whose data interval contains the checked address. That
distinguishes the candidate heap from other 5 MiB allocations. For an actual
pre-retirement write, record its CPU writer PC and resource owner, then
trace that caller's label/flip predicate. The present log has neither and
cannot close the writer/reuse question without additional evidence.
