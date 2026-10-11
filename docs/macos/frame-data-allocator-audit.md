# Fetch-table allocation and the alleged frame ring

Static audit of the 01.09 image, 2026-10-11. PCs are image offsets, added to
`0x800000000` at runtime. No game was launched for this audit.

The concrete allocator found here is **a free-list arena with 64 KiB bump
suballocations**, not a byte-wrapping 128 MiB ring. Its normal retirement is
an EOP value-4 label. Identifying its live bounds with the bad table remains
necessary; the initializer probe below supplies that evidence.

## Evidence and ownership boundary

Playtest's `20261011-192653-1080p-HUNT-1/game.log` stops on a 16-byte
instrumented UD-table read at `0x70437a72a0`, shader `0xb10c4f55`. It is not
inside a held command region or either assumed A1/A2 command pool. The log
maps the containing large direct allocation at `0x7042c00000`, size
2398093312, protection `0x33`. This does not identify the suballocator.

The proposed `[0x7043000000,0x704b000000)` ring and approximately 2.7-frame
capacity came from an address-range interpretation of earlier buffer
observations. HUNT-1 does not establish those boundaries or a wrap rule.
The `20261011-191707-1080p-R0-perf/game.log` stop occurred with labels at
completion and explicitly enabled flips at retirement. That rules out
enqueue-time flip publication as a necessary condition of that failure,
but does not identify the writer or lifetime predicate.

There is a matching 128 MiB guest allocation path:

| PC | Operation |
| --- | --- |
| `0x1d9933d` | Default graphics settings store `0x8000000` at settings+`0x88`. Settings+`0x8c` independently receives the same size; do not conflate them. |
| `0x21994910..0x2199494f` | Renderer creation copies settings+`0x88` into context configuration+`0x54`. |
| `0x216b207..0x216b220` | Context initialization calls its supplied payload allocator, vtable+`0x58`, for that size with alignment 16; saves the base at owner O+`0x10`. |
| `0x216b258..0x216b274` | Saves a resource allocator A at O+`0x18`; calls `0x26a9b50(A, metadata_allocator, payload_base, size)`. |
| `0x26a9b7d/0x26a9b88` | Initializer stores the aligned payload bounds at A+`0xb8`/A+`0xc0`. |

The primary renderer's candidate owner can be found through
`R = *[image+0x55406c8]`, `O = *[R]`, `A = *[O+0x18]`.
These pointers and bounds must be safely read and validated in the actual
run; their presence is not proof that every graphics context shares A.

The existing entry probe needs no new code to capture initialization:

```sh
BB_GUEST_ENTRY_TRACE=0x26a9b50
```

Leave `BB_GUEST_ENTRY_ARM` unset: delayed movie arming misses initialization.
The entry is `55 48 89 e5`. Each record supplies RDI=A, RDX=payload base,
RCX=size. Playtest has this exact configuration for the next stop hunt.
Do not substitute the earlier run's bad address for the next run's address
without checking layout stability.

## Allocation does not wrap blindly

`0x26a9d50` locks A's CPU mutex, calls `0x26a9db0` with alignment 16,
and unlocks. The latter rounds the request to 16 bytes and traverses the
free-range list rooted at A+`0x90`, with sentinel A+`0x68`:

- A node N describes `[N+8, *(N+0x18)+8)` in terms of the payload-start
  values stored in this node and its next physical node.
- `0x26a9df0..0x26a9e2f` checks the aligned available interval against
  the requested size. Insufficient space advances to the next free node.
- A successful allocation splits surrounding free space as needed,
  removes N from the free list and writes N[0]=2 at `0x26a9f7e`.
- Reaching the sentinel returns null. There is no modulo-capacity rewind,
  frame-number comparison, flip-status check or unconditional overwrite.

Small transient data and tables use `0x26addc0`. Its RDI is a per-context
bucket structure T, whose first qword points at A. Requests larger than
64 KiB fail. T+`0x90` is a bucket mask; T+`0x08..0x80` contain lists of
partly consumed blocks, classified by remaining capacity. For a node N:

```
result = N.payload_start + N.used_bytes
N.used_bytes += request_size
```

Here N[0] is the used-byte count while the block belongs to the context;
it is not yet a pending completion label. Full blocks move to T+`0x88`.
The code selects a bucket that fits, reclassifies remaining space, and
never bumps beyond 64 KiB. If no bucket fits, it requests a new 64 KiB node
through `0x26a9d50` at `0x26ade0f`. On failure it calls `0x26aa860` to
reclaim completed nodes and retries once at `0x26ade2f`; a second failure
returns null. A large frame can exhaust this allocator, but the audited
allocation path does not wrap onto a still-owned block to satisfy it.

## Concrete fetch-table producer

`0x26cfcf0` builds the fetch shader's vertex-buffer descriptors. Its first
argument is the context's cached graphics state D, and its second is the
wrapper W whose command buffer begins at W+`0x10`.

1. `0x26cfeeb` and `0x26d0142` copy descriptor templates into D+`0xa260`
   slots, then update buffer base, stride, record count and memory type
   through the descriptor helpers. Dynamic vertex data can be resolved
   through `0x26d0ff0`.
2. `0x26d01b2..0x26d01cd` computes
   `first = byte[D+0x28cc]`, `count = byte[D+0x28cd]`, and allocation size
   `(first+count)*16`.
3. `0x26d01ee` calls `0x26addc0(W+0xb3e0, &command_buffer, size)`.
4. **`0x26d020c` is the payload writer:** imported memcpy copies
   `count*16` bytes from `D+0xa260+first*16` into `allocation+first*16`.
   The leading slots before `first` are not initialized by this copy.
5. `0x26d023e -> 0x1076390` emits the allocation base into shader user
   data. The latter writes a four-dword `SET_SH_REG` packet, header
   `0xc0027600` with the command-buffer shader-type bit, register selector,
   pointer low32 and high32. The register index comes from shader metadata,
   not a hard-coded s[2:3] for every shader.
6. The alternative `0x26d025b/0x26d026b` stores the pointer into the
   context's spilled user-data area and marks its state dirty. Provenance
   for direct SH writes alone does not cover this route.

Other descriptor-table paths also use `0x26addc0`, including calls at
`0x26c07f9`, `0x26c08df`, `0x26c0987`, `0x26c0a10` and `0x26c0aa3`.
`0x26adbd9 -> 0x26adc00` allocates and copies resource-update bytes through
the same small-data allocator. Thus recycled blocks can contain ordinary
float payloads as well as V# tables. This identifies possible producers;
it does not identify the producer of HUNT-1's observed float words.

## What makes the blocks reusable

The normal retirement paths are explicit, not based on F modulo B:

- `0x26adf50`, at `0x26ae66e/0x26ae6be`, retires the partial and full
  small-data block lists through `0x26aa5a0`, then clears those list heads.
- `0x26af700` likewise retires context blocks at
  `0x26af78e/0x26af7de` when its existing callback/list guards pass.
- `0x26aa5a0` enqueues N on A+`0x98`, CPU-stores N[0]=2, and calls
  EOP builder `0x10778f0` at `0x26aa60a`, with event `0x28`,
  data selector 1 (32-bit), target N and value **4**.
- `0x26aa620` is the parallel list variant. `0x26aa6a0/0x26aa780`
  either emit the same local EOP or store an external label pointer OR 1.
  That external-label branch needs its caller's producer identified.
- Reclaimer `0x26aa970..0x26aa984` accepts only low32 equal to 4, read
  inline from N[0], or indirectly through N[0]&~1. It coalesces ranges
  and restores free-list membership. The inline label is in metadata,
  separate from the 64 KiB payload.

No WRITE_DATA, RELEASE_MEM or CPU store of 4 appears in the audited direct
`0x26aa5a0` retirement path. With completion-ordered EOP handling, its own
write cannot precede parsing draws earlier in that same stream. It can
precede a later draw that incorrectly retains the old pointer, or work on
another stream whose use was not covered by that EOP. The immediate frees
and ownership-transfer limits in
[throttle-guest-lifetimes.md](throttle-guest-lifetimes.md) remain relevant;
this audit has not proved them unreachable for the bad block.

## Consequence and next discrimination

If the initializer proves that A contains the stopping table, a large
frame simply wrapping into an unparsed frame is not the mechanism in this
allocator. The useful timeline is instead **allocate block, publish table
pointer, retire block, reclaim/rewrite block, consume pointer**.

The guest's `flip_arg >= F+1-B` test is a separate frame-end gate. Its
existence and the three flip progress marks do not prove the lifetime of
an individual node, and the renderer may allocate data before that gate.

For a stop, compare the guest packet that most recently published the
table pointer, parser-applied SH/CE state and any prepared-draw copy. An
old pointer alone is not proof of an emulator defect: registers can
legitimately persist. Evidence of a missed/reordered update, or use of a
node after its actual retirement, is needed. Likewise, uninitialized
leading descriptor slots are meaningful only if the consuming shader
actually selects one of them. No production workaround follows solely
from the address-range observation.
