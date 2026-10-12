# Guest ownership of SDK fill 0x8b355b5a

Static 01.09 audit and existing P15b census, 2026-10-11. No game, build or
GPU test was launched. PCs below are image offsets, added to `0x800000000`.

**The concrete connection to the frame-data arena is the fill's 16-byte
colour input, not its write target.** The guest issuer copies the colour
through `0x26addc0`, the same small-data allocator used for descriptor tables.
Its target comes separately from a supplied resource. In the examined census,
all 252 colour inputs lie within the observed arena bounds and none of the
13 distinct target ranges overlaps them. This does not prove absence of
cross-lifetime reuse, physical aliases, or stale resource descriptors.

## Shader identity and guest issuer

The corpus entry `000000008b355b5a-cs.bin` identifies the shader container at
SELF file offset `0x52e22f0` (`Shdr` at `0x52e2314`). Loading the SELF through
`scripts/link_modules.py:module` maps its container to image `0x52ba500` and
its first 104 code bytes to `0x52ba564`; those bytes match the corpus code.
Do not interpret the corpus's file offset as a guest address. The earlier
`/tmp/pm4_re/image.bin` contains only the executable segment, so a search for
this shader there alone misses the data-segment program.

Initializer `0x26b2030` registers this container through `0x2172db0` at
`0x26b215e`, supplying shader object `image+0x546be70`. The object's two
observed code references used for dispatch are both in **`0x26b4ef0`**:

| PC | Operation |
| --- | --- |
| `0x26b51be` | Calls `0x26cd400` with shader object `0x546be70`. |
| `0x26b52a8` | Calls `0x26cd5d0` with the same shader object. |
| `0x26b52bc` | Calls dispatch wrapper `0x1074c50`, dimensions X calculated from the target element count, Y=Z=1. |
| `0x26b52ce` | Calls post-dispatch helper `0x26ce110`. |
| `0x26b531d` | Calls the existing guarded context flush/retirement helper `0x26af700`. Its guards and lifetime limits still apply. |

Six direct CALL sites to `0x26b4ef0` were found and disassembled:
`0x2168648`, `0x2198f53`, `0x2263634`, `0x2263697`, `0x22636fa`,
`0x22908b7`. This is a direct-call census, not proof against indirect calls.
The first is in wrapper `0x2168630`, itself called from numerous render
paths (including `0xe48c78`, `0xeab451`, `0xf2e2f4..0xf2e39c`). The three
`0x22636xx` calls obtain distinct supplied resource handles and use local
float4 colours. `0x22908b7` uses a float4 constructed from a packed colour.
The `0x2198f53` path supplies a renderer-owned colour at another object+`0x120`.
All six retain the same target-resource versus colour-input distinction.

## Colour storage and target storage are independent

At `0x26b4ef0` entry, RDI is context C, RSI is view metadata V, RDX is
resource metadata Q, and RCX points to the input colour.

The normal shader branch allocates **16 bytes** at `0x26b501c` through
`0x26ae7f0`. That helper calls `0x26addc0(C+0xb3e0, &CB, 16)` at
`0x26ae81c`. Allocation failure exits without issuing this fill. The two
qword stores at `0x26b5036/0x26b503a` copy the caller's colour into that
allocation. The colour V# is built at `0x26b50a5` and stored in context
state at `C+0xa170`. This establishes the guest producer of the colour
payload and its relationship to the allocator in
[frame-data-allocator-audit.md](frame-data-allocator-audit.md).

The **target** takes another route:

1. `0x26b51eb -> 0x26baf60` derives an offset and byte size from Q and the
   selected subresource. Failure skips that iteration.
2. `0x26b5213/0x26b5217` computes `target_base = [Q+8] + offset`.
3. `0x26b5228 -> 0x10707c0` builds the target V# from that address, the
   selected format and the derived record count; `0x26b5256/0x26b525a`
   writes it into context state at `C+0x9330`.

There is no target allocation from C's small-data allocator in this path.
The helper is nevertheless **not limited to an RT address class**: view
kind 1 uses a single iteration, and `0x26baf60` resource kind 1 accepts
subresource zero with offset 0 and byte count `[Q+0x14]`, a buffer-like
whole-range case. Other resource kinds calculate surface/subresource
layout. This establishes an address-supplied resource fill, not that all
possible callers provide disjoint render targets. The earlier metadata
branch through `0x26b4f9f -> 0x26b5380` uses a different path and is not
the `0x8b355b5a` dispatch identified above.

## Existing range evidence

Source: Renderer's worktree `thr_9dn45wgcj3-1/bloodborne_pc`,
`out/macos_route/20261011-091134-4k-P15b-rtcensus3/game.log`, lines containing
`RT kernel census: FILL`. A fresh offline parse found 252 recorded call
shapes, 252 read buffers inside `[0x7043400040,0x704d400040)`, and zero
write-buffer intersections with that interval. The census deduplicates call
shapes; these are not counts of all executed fills. Arena bounds were
established in a separate initializer-probe run, so this numerical comparison
does not replace same-run allocator ownership or backing-alias evidence.

| Target base | Bytes |
| --- | ---: |
| `0x704ff30000` | 15,073,280 |
| `0x706fa38000` | 3,932,160 |
| `0x70714c0000` | 30,146,560 |
| `0x7075a70000` | 16,777,216 |
| `0x707c700000` | 30,146,560 |
| `0x7080080000`, `0x7081d40000` | 15,073,280 each |
| `0x7082ba0000`, `0x7084860000`, `0x7094c00000` | 30,146,560 each |
| `0x71497e0000`, `0x71497e8000`, `0x71497f0000` | 32,768 each |

These are guest kernel calls, not proof each qualified for the later image-clear
replacement. The replacement additionally requires exactly one cached colour
image with the exact target base, size and format (one mip/layer/sample), and
an exactly representable repeated-word colour. It writes only the target V#
range with `vkCmdFillBuffer`, then clears the separate cached image. Shader
identity alone neither authorizes arbitrary ranges nor establishes image
lifetime correctness.

## Can the target later become frame-data memory?

While a given arena remains live, ordinary `0x26a9db0` allocation splits its
existing free intervals; it does not acquire arbitrary render-target ranges
or grow its payload bounds. The bounds are supplied once to `0x26a9b50`.
Thus a disjoint target cannot enter this live arena through ordinary node
reclamation. Conversely, small-data reclamation makes payload available to
other allocations within that same arena; it does not return the complete
160 MiB backing allocation to an external render-target allocator.

That is a scoped ownership conclusion, not an all-lifetimes exclusion:
the outer allocator may reuse addresses after arena destruction; a caller can
supply a resource view over arena memory; corrupted/stale Q or V# values can
name the wrong range; and distinct virtual ranges can alias backing storage.
The inspected issuer and census do not establish any of those occurring.
Proving later RT-to-arena transfer requires the outer allocation/free or
mapping history for both resources, not their common large VMA.

The actual shared-arena input raises a separate timing question: the
replacement reads its colour on the CPU at recording time, whereas the
original kernel reads it during GPU execution. Normal inline EOP-4 retirement
after the fill should protect that allocation until GPU completion under the
current label mode; alternate-free and stale-state exceptions remain as
documented in the allocator audit. The supplied samples do not show a colour
rewrite during this interval. A repeated-word buffer fill also does not, on
its own, explain a descriptor containing several different float words.

Bit-62 A/B can establish that the replacement affects the failure rate. It
does not alone distinguish a colour-snapshot timing change, a stale image made
authoritative by the clear, synchronization differences, or a misidentified
write target. Those remain renderer-side questions. No direct fill into the
observed frame-data arena is established by this audit.
