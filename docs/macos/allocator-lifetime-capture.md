# Held allocator lifetime capture

Prepared 2026-10-11. **Hold for PM4's compute-queue SET_SH_REG result before
sending this recipe to Playtest.** No game was launched for this preparation.
This uses the existing entry probe and an external log collector; it changes
no runtime code. It is a partial lifetime observation, not a complete
allocate/retire/reclaim trace.

## Exact environment

Add these to Playtest's next approved stop-hunt launch, from boot. Keep the
normal hunt's snapshot and descriptor diagnostics. Do not arm at movie start:
this capture needs initialization and gameplay allocation history.

```sh
unset BB_GUEST_ENTRY_ARM
export BB_GUEST_ENTRY_TRACE=0x26a9b50,0x26addc0,0x26a9db0,0x26aa5a0,0x26aa620,0x26aa6a0,0x26aa780,0x26aa860,0x26aa2e0,0x26aa440,0x10778f0,0x26b0b00,0x26b0e30
export BB_CMD_DATA_CHECK=1
export BB_LABEL_CHECK=1
export BB_WRITE_LOG=2
export BB_LABELS_AT_GPU_COMPLETE=1
export BB_FLIP_AT_GPU_RETIRE=1
```

The explicit completion/retirement settings avoid historical toggle-bit
inversions. Check the startup mode lines and all 13 installed-site lines.
All sites were checked against the extracted 01.09 image and begin
`55 48 89 e5`; they fit the existing 16-site limit. No claim is made about
another image version. The loader still validates executable placement and
the supported first byte at startup.

Once this run's `game.log` exists, attach the external collector:

```sh
python3 tools/macos/guest_lifetime.py /absolute/run/path/game.log \
  --follow --capacity 8192 --output /absolute/run/path/lifetime-stop.jsonl
```

The output path must not exist. The collector reads from the beginning,
incrementally, retaining at most 8192 selected entry records. It dumps at
`STOP:` or `Guest fault BB_FAULT_DUMP`. Ctrl-C writes an explicitly interrupted
window. Without `--follow`, it also supports an existing completed log and
labels a normal EOF accordingly. A process killed without those stop markers
needs Ctrl-C or an offline pass; the collector does not watch the game PID.

This is an **external** ring over the existing streamed log, not a new signal
handler ring or a new stop hook. The header gives total/retained/evicted counts
and all-run site counts. It does not reduce the game's raw log size or entry
trap overhead: `0x26addc0` can be frequent. These runs are unsuitable for perf
measurement. Keep the raw log for history older than the retained window.

## Site interpretation

Each raw record has sequence, Mach ticks, thread handle, pthread identity/name,
caller, all GPRs and eight qwords at RDI/RSI/RDX. Sequence marks handler entry,
not completion of the guest operation. Reads are not coherent snapshots.

| Offset | Meaning and visible arguments | Missing evidence |
| --- | --- | --- |
| `0x26a9b50` | Arena initialization: RDI=A, RDX=payload base, RCX=size. | No per-node allocation result. |
| `0x26addc0` | Small allocation request: RDI=T, `[T]=A`, RDX=bytes, RSI=command-buffer reference. | Returned payload and selected node. Only the first 64 B of T is captured, not all bucket heads/mask. |
| `0x26a9db0` | Node-grab request while the arena lock is held: RDI=A, RSI=bytes, RDX=alignment. | Selected/result node: selection happens after entry. Do not call an entry a successful allocation. |
| `0x26aa5a0`, `0x26aa620` | Inline retirement entry: RDI=A, RSI=command-buffer reference, RDX=N. `rdx-words[0]` is the pre-entry node word; `[1]` is payload base, `[2]/[3]` previous/next physical node. | Publication CAS success, N[0]=2 store, and label completion all occur later. |
| `0x26aa6a0`, `0x26aa780` | Same RDI/RSI/RDX plus RCX=optional external-label address. Zero selects the inline-label path. | External label contents are not read by this entry probe. |
| `0x26aa860` | Reclaim scan begins: RDI=A, RSI=command-buffer reference. | Which nodes passed low32==4, whether any were freed, or their contents at that decision. The loop is inline, not another framed function. |
| `0x26aa2e0`, `0x26aa440` | Immediate-free entry: RDI=A, RSI=N; `rsi-words` exposes the same node header. | Exact mutation/completion time; entry occurs before taking the arena mutex. |
| `0x10778f0` | EOP builder entry: RCX=label address, R8=data selector, R9=value. Match inline retirements by RCX=N, selector 1, value 4, and caller. | This is packet construction, not submission, execution or label write. |
| `0x26b0b00` | Submit-wrapper entry. | It may perform no submit or use a different path; this is not a parser submission ID. |
| `0x26b0e30` | Frame-end wrapper entry, before SubmitAndFlip/SubmitDone. | Not a displayed frame or GPU retirement. |

The collector attaches global **observed wrapper-entry ordinals** to each
record. These are coarse log-arrival stamps, not exact guest frame numbers or
PM4 submission IDs. Concurrent writes can reach the log in a different order
from sequence assignment. Raw sequence/ticks/caller remain available; never
use these ordinals to assert cross-thread happens-before. Actual submission
identities must come from the stop's parser provenance and write-log records.

## Matching the bad table

1. Obtain the bad read address and size from this descriptor's stop provenance.
   Do not substitute another descriptor's latest read or an earlier run's address.
2. Find retirement/free records whose observed payload base can contain that
   address. For a node positively tied to a small allocator's 64 KiB grab,
   test the 64 KiB interval; do not assign that size to arbitrary arena nodes.
   The entry records contain the next physical **node pointer**, not its payload
   value, so they do not directly provide arbitrary node interval ends.
3. Join by N and payload address, retaining caller/thread and temporal order.
   Node addresses can be recycled or coalesced: N alone is not a lifetime ID.
   A retirement entry followed by its EOP-builder entry establishes label
   construction for that N, not completion. An immediate-free entry for N is
   a concrete alternate-free candidate. Reclaim-entry adjacency proves neither
   that N was inspected nor that it was accepted.
4. Compare against the packet that published the consumed table pointer and
   any observed GPU label write. `BB_LABEL_CHECK` reports changes before a
   checked write, not every legal completion. `BB_WRITE_LOG` parse records
   likewise must not be reinterpreted as completion timestamps.

The existing probe cannot establish the pending-publication race: both the CAS
and the node predicate are inline after entry, and there is no return probe.
It also cannot give a complete successful-allocation history. In particular,
a pre-retirement word of 4 plus a nearby reclaim entry is insufficient proof
that reclaim read 4 between CAS and initialization. If the compute-register
investigation does not settle the stop, this capture first tests whether the
bad block has an observed immediate-free path and identifies its retirement
caller/node. A definitive allocation/reclaim/predicate timeline still needs
more targeted instrumentation; no supported entry-only environment supplies
those missing observations.
