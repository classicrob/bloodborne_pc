# Opening-movie resource-index fault at 0x26ae211

The new fault is a write through a guest graphics resource's per-context index
table. It is not the earlier `movie+0xe0` vertex-shader pointer dereference.
Both faults occur in CPU render-state processing, but their pointer chains and
lifetimes differ. The available logs do not prove a common cause, stack
corruption, or a dependency on COND_EXEC/EOP writes.

## Evidence

Static audit of the read-only EU v1.09 eboot, SHA-256
`d65f0b4f01d59166aed16f8604196d8b7dd805abbf0758b356e8f1354c9429f9`,
using small SELF executable-segment windows and native LLVM disassembly. No
new game process or debugger was used. Addresses below are image-relative.

Production acceptance artifacts: `efce883`, probe SHA prefix `60af90a5`, GPU
library `1c60558c`, default configuration plus `BB_CMD_SNAPSHOT_CHECK=1`.
Playtest logs:

- `20261010-200932-1080p-ACCEPT4-2-efce883/game.log`, line 4278.
- `20261010-201203-1080p-ACCEPT4-3-efce883/game.log`, line 2943.

Both report signal 11 at offset `0x26ae211`, address zero, and the same caller
chain. Neither provides general-register values or resource addresses.

## Faulting pointer

The function begins at `0x26adf50`. Its arguments are a resource-entry list and
render context. On this call the list is `context+0xb3c0`; R14 holds the context
and R11 the list at the fault. It walks chunks of 128 entries, each 16 bytes:
resource pointer followed by its associated data pointer.

| Offset | Operation |
| --- | --- |
| `0x26ae0e7` | Load resource from `entries + 16*index`; skip a null resource. |
| `0x26ae0f4` | R9 = address of that entry. |
| `0x26ae1ff` | RAX = unsigned 16-bit context ID at `list+0x18`. |
| `0x26ae204` | Skip the index-table write if ID > 15. |
| `0x26ae20a` | RCX = resource pointer loaded again from `[R9]`. |
| `0x26ae20d` | RCX = resource's pointer at `+0x38`. |
| `0x26ae211` | Store unsigned 16-bit zero at `[RCX + 2*RAX]`. |

The failed access is therefore `resource->context_indices[context_id] = 0`.
That field naming is inferred from the paired lookup and cleanup code below,
not recovered source symbols. With an ordinary low heap pointer, fault address
zero means a null table and context ID zero. Formally the log establishes only
that `RCX + 2*RAX` is zero and `RAX <= 15`; without registers it cannot exclude
an already-corrupted high pointer wrapping to zero.

`0x26ae840` performs the inverse lookup: for context IDs <= 15, it reads the
same resource `+0x38` table, subtracts one from the 16-bit index, checks it
against list size, and checks that the indexed list entry points to this resource.
Larger context IDs use a linear search instead. This identifies the table as
CPU-side resource lookup metadata, not a GPU label, semaphore result or AvPlayer
handle.

## Normal producer and teardown

Resource initializer `0x216d420` builds the matching resource record. Relevant
fields include data allocation at `+0x08`, byte size at `+0x14`, flags at `+0x23`,
index-table allocator at `+0x30`, and index-table pointer at `+0x38`.

- `0x216d574` initializes the table pointer to zero.
- `0x216d57c` tests descriptor byte `+0x08` with mask `0x04` (bit 2), copied
  earlier into resource byte `+0x23`. Without that flag the table stays null.
- With the flag, `0x216d596` calls the supplied guest allocator's vtable `+0x50`
  entry with size 32. `0x216d599` publishes its pointer at resource `+0x38`,
  and `0x216d5a7` through `0x216d5bf` zero all 16 unsigned-short slots.
- Missing allocator returns error 5; allocation failure selects error 9 and
  enters cleanup. Those error paths are not evidence that either occurred here.

Destructor `0x216d690` first frees the resource data allocation and clears
`+0x08`. If resource flags contain `0x04`, it walks the 16 context pointers
in the guest global array at `0x56a63c0`. For each present context,
`0x216d6e7` calls `0x26ae900(context+0xb3c0, cached_index)`. That helper checks
the one-based index and clears the list entry's resource and data pointers.
The destructor then frees the index table via the allocator at `+0x30`, and
clears `+0x38` at `0x216d711`. Initializer error cleanup has a similar sequence.

These accesses establish a CPU allocation/publication/removal lifecycle.
Concurrent list traversal versus teardown/reuse is a concrete hypothesis: the
walker can observe an entry before removal and then a cleared/reused table.
The local destructor/list operations shown contain no lock; callers may provide
synchronization, so this is not proof of an unsynchronized guest operation.
A resource created without the flag reaching the cached-index path, or arbitrary
resource/list corruption, remains possible. There is no identified GPU producer
for the table pointer. A GPU completion bug could still affect its lifetime
indirectly; that requires live evidence.

## Callers and the float-shaped backtrace entry

All four reported guest return addresses match direct calls:

| Return offset | Call site and target | Role supported by the code |
| --- | --- | --- |
| `0x216aa85` | `0x216aa80 -> 0x26adf50` | Flush context resource list before processing the next queued render object. |
| `0x21b4d9c` | `0x21b4d97 -> 0x216a980` | Process accumulated render objects before a queued callback. |
| `0x2199bbe` | `0x2199bb9 -> 0x21b4d00` | Dispatch queued render-work entries. |
| `0x21b69d5` | `0x21b69d0 -> 0x2199af0` | Worker processes its pending object; clears worker `+0x18` after return. |

The source types and names are unknown. This chain does not contain the movie
draw routine `0x28fe7c0` from the earlier fault, although it is reached while a
movie is playing.

`probe.c` prints these numbered frames with host `backtrace()` from the signal
handler. It does not start a guest frame-pointer walk from the saved ucontext.
Thus `#2 = 0x3f24c0003f24c000` is an untrusted unwind result across a signal and
guest code without registered host unwind metadata. It is not yet evidence
that the actual guest `[RBP+8]` return slot contains those two floats. Inspecting
the saved guest frame is needed to distinguish unwind artifact from corruption.

## Next diagnostic

At this exact fault, capture saved RAX, RCX, R9, R11, R14, RBP and RSP. Use
Mach-safe reads for the entry at R9, the resource's first 64 bytes, the list
header at `R14+0xb3c0` and the saved `[RBP,RBP+8]` frame pair. Verify that R11
matches that list, and record the global context array slot for its ID.

The critical discriminator is resource byte `+0x23` mask `0x04` versus table
`+0x38` and data `+0x08`: a live resource without the cache flag differs from a
partially destroyed one. Once its address is known, correlate initializer and
destructor entry/exit with the queued render object's lifetime. Match any
COND_EXEC/WRITE_DATA/EOP diagnostic to these concrete addresses before assigning
causality. No guest patch, null-table bypass or rendering repair is justified
by this static evidence alone.
