# Tessellation constants in command chunks

Static producer and lifetime audit for the RING-17b read-check residual associated with
`vs 0x4cd21a0f`, stage 2 binding 0, 48 bytes. The logged VS hash identifies the draw, not
necessarily the shader that consumes this binding: the verifier identifies that hash as LS
and is separately checking the TES loads. Image offsets below use the same Bloodborne 01.09
image and address convention as [throttle-guest-lifetimes.md](throttle-guest-lifetimes.md).

## Evidence and address classification

Log: Playtest worktree `out/macos_route/20261011-172926-1080p-RING-17b/game.log`.
Filtering `changed between`, `stage 2 binding 0 vs 0x4cd21a0f`, and `+48` gives 52 records.
The addresses range from `0x703b47fff4` to `0x70412ffff4`.

The previously captured A2 pool has base `0x703b400020`, 384 chunks of `0x40000` bytes,
and exclusive end `0x7041400020`. All 52 addresses fit this layout, in chunk indices 1..379:

| Hits | Offset from actual chunk base | Interpretation |
| --- | --- | --- |
| 45 | `0x3ffd4` | Exactly 36 bytes before the chunk's status qword at `+0x3fff8` |
| 7 | `0x33ee4` | Interior of the same 256 KiB layout; all seven share this offset |

This is an address match to the prior pool capture, not a new runtime read of the allocator
header in RING-17b. The interior addresses' common suffix `...3f04` alone does not establish
a separate 16 KiB allocator. They are compatible with end allocation after `0xc0f0` bytes
were already reserved; this audit has not identified those preceding allocations.

For a rounded 256 KiB boundary B, the 45 ranges are `[B-0xc, B+0x24)`:

| Bound-range offsets | Address | Contents under that pool layout |
| --- | --- | --- |
| 0..35 | `B-0xc .. B+0x17` | The 36-byte tessellation constants |
| 36..43 | `B+0x18 .. B+0x1f` | This chunk's 64-bit status |
| 44..47 | `B+0x20 .. B+0x23` | First dword of the next chunk |

Thus a whole-48-byte hash can change because of allocator metadata or a neighboring chunk,
without any change to the constants. Changed-byte offsets are required to distinguish this.
The seven interior ranges do not have this particular status-word overlap.

## Static producer PCs and the 36-to-48-byte expansion

Both paths below are in the large state-building function at `0x26bc320`. At its entry,
RSI is the context wrapper; the primary CommandBuffer is RSI+`0x10`, saved locally at
`0x26bc39b/0x26bc39f`. The paths generate the nine-word constants on the stack through
`0x1087db0`, then perform:

| Operation | Path A | Path B |
| --- | --- | --- |
| Generate the nine-word constants | call `0x26c26b5` | call `0x26c7b1e` |
| Allocate 36 bytes, alignment 4, from the CB | call `0x26c26ce -> 0x26b07f0` | call `0x26c7b37 -> 0x26b07f0` |
| Write exactly 36 bytes to the returned address | stores `0x26c26d9`, `0x26c26e3`, `0x26c26ee`, `0x26c2700`, `0x26c2704` | stores `0x26c7b42`, `0x26c7b4c`, `0x26c7b57`, `0x26c7b69`, `0x26c7b6d` |
| Build the V# with requested size `0x24` | call `0x26c2713 -> 0x1070960` | call `0x26c7b7c -> 0x1070960` |

`0x1087db0` writes nine dwords at offsets 0..32, including `0x44000000` at +28.
The copy sites use four qword stores and one dword store; no padding bytes are initialized
there. Descriptor builder `0x1070960` writes stride 16 and sets num_records to
`(requested_bytes + 15) >> 4`. A request of 36 therefore exposes three records, or 48 bytes.
It sets descriptor word 3 to `0x20077fac`. Both callers copy the descriptor into state at
`+0x9d50` and `+0x9e90` and set the corresponding dirty bits.

These are statically identified producer paths matching the nine-word structure and rounded
descriptor. The RING-17b hash log does not contain CPU writer PCs, so it does not establish
which branch produced each individual hit. A targeted trace can distinguish them if needed.

## Allocation and submission boundaries

`0x26b07f0(CB, bytes, alignment)` reserves downward from CB.end (+8), while packet writes
grow upward from CB.cmdptr (+16). It rounds bytes to dwords, includes alignment padding,
requires the new end to remain strictly above cmdptr, and calls the CB's out-of-space callback
when necessary. At `0x26b08be` it replaces end with the allocated address and returns it.
For the 36-byte, four-aligned request at a fresh chunk end, this returns status-address minus
36 exactly. It does not advance cmdptr or wait for a flip.

These bytes are in the same physical chunk allocation as commands, but are **not the
submitted DCB/CCB packet span**. The normal submit helper `0x26b0b00` forms sizes from
cmdptr-begin (`0x26b0b8b..0x26b0bc9`) and submits those sizes at `0x26b0c23`. Constants
reserved above the lowered end are addressed separately through the V#. Submit snapshots
of packet spans do not inherently copy these end allocations.

## Reuse predicate and status writers

The ordinary chunk protocol is independent of a flip test inside the allocator:

1. `0x26d3b80` scans chunk statuses and claims a zero with a 64-bit CAS from 0 to 1
   (`0x26d3c1f -> 0x207ebc0`). It cannot claim status 1 or 2.
2. Graphics release `0x26d3c80` CPU-stores 2 at `0x26d3c8c`, then emits an EOP qword-zero
   write through `0x26bb6d0`. The secondary/compute release `0x26d3cb0` stores 2 at
   `0x26d3cc4`, then emits RELEASE_MEM with Data64/zero.
3. The completion write changes status to 0, making the whole chunk available to the next
   claimant. Correctly ordered GPU-completion labels protect these addressed constants as
   well as packet storage. The nonblocking claim can fail; its caller decides whether to
   retry/yield. The child callback `0x26af230` returns failure if no chunk is available.

Status writes can legitimately change bytes 36..43 of the 45 tail ranges during release,
completion, or a later claim. Reuse of the following chunk can change bytes 44..47
independently. There is also the separate direct-reset store at `0x26d3d34` and pool teardown;
their unresolved caller-lifetime conditions remain in the linked audit. They must not be
silently folded into the ordinary completion-governed protocol.

The read checker samples once at record and again **after** observing GPU retirement.
Even changed bytes 0..35 would require timing attribution: lawful reuse after retirement
but before the CPU checker reads is not proof that the GPU consumed overwritten data.
Conversely, unchanged hashes do not rule out an intervening rewrite restored before checking.
Renderer owns the changed-byte diagnostic; verifier owns the actual TES read extent.
No production fix or new game run follows from this static audit alone.
