# Deferred-signal waits: attribution and guest dependencies

Source-only audit of macos `8c71a95`, plus the 01.09 guest image described in
[throttle-guest-lifetimes.md](throttle-guest-lifetimes.md). No policy change
or new game run. PM4 owns the optimization design.

## The measured counter does not identify WAIT_REG_MEM

The R0 walk capture is Playtest's
`out/macos_route/20261011-175746-1080p-R0-perf/game.log`, walk section
approximately lines 14958–30301 (`segments.log` defines the boundaries).
The reported 15–25% uses global `BbStats::deferred_signals_wait_ns`: a sum
of calls on multiple threads, including stage B. It is neither a per-packet
census nor an additive partition of frame wall time.

Graphics WAIT_REG_MEM is classified as pipelined. Its handler at
`liverpool.cpp:1876` uses a VideoOut-label sleep, pending-value forwarding,
or live poll/drain/yield. Compute WAIT_REG_MEM at line 2344 polls/yields.
Neither directly calls `WaitDeferredSignals`. A live poll can still depend
on GPU completion, but that alone does not charge this counter.

The important explicit callers are:

| Path at the pinned source | What it waits before doing |
| --- | --- |
| Graphics non-pipelined opcode head, line 1379 | Executing a packet outside the pipelined set, after draining stage B; includes COPY_DATA, MEM_SEMAPHORE and COND_EXEC |
| Stage-B `RunWriteData`, line 851 | A host-visible memory store, also after host copies |
| Stage-B `SignalFlip`, line 841 | Delivering the GfxFlip callback |
| Compute WRITE_DATA, line 2316 | Its host-visible write |
| Compute RELEASE_MEM, line 2360 | Processing the release, **before** enqueueing its own GPU-completion callback |
| Final GpuIdle, line 462 | Idle publication after the drain |
| Mode switch / legacy EOP/EOS / GDS paths | Transitioning label mode or immediate/explicit-finish paths; these are not evidence of steady completion-mode scratch waits |

`SyncParserRead` also waits when a host parser read overlaps a pending fence;
COND_EXEC is one consumer. Its dependency is CPU-visible, not merely later
GPU execution. In a late R0 600-flip window (log line 28954), GpuIdle's own
recorder/label waits round to zero while the global deferred field remains
large nearby. This narrows the likely phase, not the packet or producer.
PM4's parse-split census is intended to attribute the next capture by thread
and packet class. Existing logs have no per-wait operand/producer records.

## Guest wait families identified statically

Image offsets below are call/build sites, not runtime packet addresses.
The generic graphics builder is `0x1077b30`; compute uses `0x108b430`.
They encode a 32-bit poll; ordinary scratch waits use full mask
`0xffffffff`, compare Equal (3), reference 1. Addresses are runtime
allocations, so static RE cannot supply a captured absolute address.

| Guest path | Polled address and predicate | Producer established by local code |
| --- | --- | --- |
| `0x26b08e0` | Scratch L reserved downward from the same CommandBuffer end; L == 1 | CPU initializes L to 0 at `0x26b0994`; EOS builder at `0x26b09a9` writes 1; wait at `0x26b09ce` |
| `0x26b09e0` | Same allocation pattern; L == 1 | CPU zero at `0x26b0a98`; EOP through `0x26bb6d0` at `0x26b0ab1`, Data64 value 1; wait at `0x26b0ad4` |
| `0x11ebc20` / `0x11ebc90` | Local scratch L == 1 | EOS then graphics wait, event types 0x30 / 0x2f respectively |
| `0x11ebd00` | Compute scratch L == 1 | RELEASE_MEM at `0x11ebd4f`, Data64 value 1; compute wait at `0x11ebd6a` |
| `0x21d857e..0x21d85ad` | L = `*[0x56c3cf8] + 8*byte[0x56a6d20]`; equal to low32 of `*[0x56c3ce8]` | Same-CB EOP builder followed by wait; absolute address/value require a capture |
| Submission helpers `0x26b0b00` / `0x26b0ce0` | Let S = `*(*(context+0xb490)+0xb728)`. Address `*[S+0x10]`, target `u32(*[S+0x18]-1)`: unsigned >= target, with a preceding unsigned < cached value on the wrap path | **Unresolved producer**. Do not infer same-queue EOP from its use as a submission counter |
| Gnm `sceGnmInsertWaitFlipDone` | VideoOut label base + 8*buffer index, low32 == 0 | Previous-buffer release in `PublishFlip`; mode selects retirement/enqueue. Close/register also affect labels, so frame and port generation matter |
| Streamout-flush register wait | Register `0xc03f`, mask 1, NotEqual 0 | Prior streamout-flush EVENT_WRITE; existing Apple path asserts its modeled state and elides the poll |

The submission helpers' wait PCs are `0x26b0b67/0x26b0b80` and
`0x26b0d4e/0x26b0d67`. Their cached value is S+`0x20`. None of these
static occurrences establishes frequency in the measured clinic window.
CPU-written labels, other compute queues and unknown counter writers must
remain distinct until a runtime producer join establishes otherwise.

## Where a GPU dependency can replace a CPU wait

The local scratch pairs are the strongest candidates: the intended producer
and comparison are visible in one command sequence. A replacement is exact
only when it establishes all of the following:

1. Identify the particular producer write, address, width and value, with
   no intervening conflicting write/reset. The predicate must follow from
   that value, not from a stale address-only cache.
2. Order subsequent GPU work after the producer with the appropriate
   execution and memory dependency. Submission order alone is not such a
   dependency. A same-queue barrier or explicitly joined timeline can be
   suitable; cross-queue work requires its actual producer queue/ticket.
3. Keep later **host** effects behind the dependency when required: parser
   reads of SRT/indirect/query data, COND_EXEC decisions, preparation reads,
   host WRITE_DATA, interrupts and guest-visible label publication. Merely
   letting the parser advance does not satisfy these consumers.
4. Submit the producer batch before waiting for its completion ticket; a
   ticket that also includes the blocked consumer creates a cycle.

These conditions follow the distinction between execution order and memory
dependencies in the [Vulkan synchronization specification](https://docs.vulkan.org/spec/latest/chapters/synchronization.html).
There is no blanket permission to skip all same-stream waits. For the
unresolved counters, CPU-produced values and VideoOut lifecycle labels,
the audit has no proof sufficient to remove the CPU dependency. A host
read that needs the completed bytes still needs completion or an exact
versioned representation of those bytes.

One existing optimization deserves care: `PendingFenceValue` stops at
`DrawPipeReached(position)` with a comment that memory already contains
the value. With GPU-completion labels, stage B reaching a packet does not
mean its label has been published. Before that point the graphics handler
may forward the predicted value; after it the same wait can fall back to
polling. Extending this cache's lifetime would make forwarding more
consistent, but would not by itself prove the host/GPU ordering above.

## Next measurement, without a separate run

Use PM4's per-class deferred-wait attribution first. If WRITE_DATA, flip or
compute RELEASE_MEM dominates, optimize that actual dependency rather than
assuming graphics WAIT_REG_MEM is the cost. A subsequent bounded census
should record queue/submission/packet PC, poll address, mask/function/ref,
matched producer packet and retirement ticket, result and elapsed time.
Keep unmatched writers explicit. That allows PM4 to select proven scratch
dependencies and leaves CPU-visible waits intact. No extra game slot is
requested by this note.
