# Opening-movie fault at guest offset 0x26c01c3

The fault is a null guest vertex-shader wrapper dereference. Its normal producer
is the guest movie object's shader-setup routine, not a GPU-written label, kernel
event result, or the opaque host AvPlayer handle. The available static evidence
does not establish why that pointer became null, or connect it causally to the
separately observed command-buffer reuse before parsing.

## Inputs and method

This audit uses EU v1.09 `eboot.bin`, SHA-256
`d65f0b4f01d59166aed16f8604196d8b7dd805abbf0758b356e8f1354c9429f9`.
`scripts/link_modules.py::module` parsed the original read-only SELF; its executable
PT_LOAD at virtual address zero (84,776,668 bytes) exactly matched the previously
extracted audit segment. Native ARM LLVM objdump decoded small windows, with each
window placed at its original guest-relative address. PLT names below were resolved
through the original relocation/symbol tables and the runtime's NID registrations.
No game was launched, debugger attached, or runtime source changed.

The matching logs in the playtest worktree are:

- `out/macos_route/20261010-182627-1080p-DIAG-inorder-writes-edd8d1f/game.log`:
  signal 11, guest offset `0x26c01c3`, fault address `0xd2`.
- `out/macos_route/20261010-182833-1080p-PRIVATE-canary-67d231a/game.log`:
  the same fault under the default pipelined configuration (line 2601). This is a
  private observing build, not production acceptance evidence.

## Pointer and call chain

At `0x26c01c3`, the instruction is `cmp byte ptr [r9+0xd2],0`. Fault address
`0xd2` therefore identifies a zero `r9`. The log has no general-register snapshot;
that register value is inferred from the instruction and reported address.

The movie draw routine starts at `0x28fe7c0`. Its first argument is a guest movie
object, called `movie` below; the second is its render context, called `context`.

| Guest offset | Operation |
|---|---|
| `0x28fe7f6` / `0x28fe806` | Read `movie+0x98`; call `sceAvPlayerIsActive`. |
| `0x28fe83e` / `0x28fe84d` | Pass that handle to `sceAvPlayerGetVideoDataEx`; return without drawing if false. |
| `0x28feb83` | Select draw state at `context+0xb730`. |
| `0x28fee81` | Read vertex-shader wrapper from `movie+0xe0`. |
| `0x28fee96` | Store it at `context+0xc1b8`, equivalently `draw_state+0xa88`. |
| `0x28ff210` | Call draw helper `0x21696c0` with context and draw state. |
| `0x216972b` | Call graphics-state wrapper `0x26bc2e0`, which tail-calls `0x26bc320`. |
| `0x26bc359` | Load `r9` from the supplied draw state's `+0xa88` field. |
| `0x26c01c3` | Read the shader wrapper's flag at `+0xd2`. |

Both return addresses, `0x2169730` and `0x28ff215`, occur in the fault log's
guest stack. The ordinary vertex-stage path preserves the shader pointer across
its helper calls. It can clear cached resource masks for a null shader, then reach
this unconditional flag read when binding state changes.

The object classification is supported by the following code, independently of
field naming: `0x26c0240` obtains the shader header from `r9+0x100`, and the helper
called at `0x26c0258` reaches PLT `0x2bc04d8`, NID `gAhCn6UiU4Y`, registered as
`sceGnmSetVsShader`. This is a guest shader wrapper, distinct from `movie+0x98`,
the AvPlayer handle.

## Known producers and lifetime

The movie constructor `0x28fd9f0` initializes `movie+0xe0` to zero at
`0x28fda82`.

Movie setup `0x28ff6a0` processes stream information using
`sceAvPlayerStreamCount` and `sceAvPlayerGetStreamInfo`. For the selected video
stream it passes `&movie[0xe0]` to the shader constructor at `0x2166d00`
(call site `0x28ff8d2`), using embedded shader data at `0x52e3c60`. The constructor
builds a temporary description, requests a `0x128`-byte, eight-byte-aligned object
through the guest allocator, copies the description, and publishes the resulting
pointer synchronously at `0x2166dc4`. Allocation failure stores zero and returns
error 9; other validation/initialization failures return an error before publishing.
The movie setup checks the constructor's return value and leaves on failure.

Two decoded callers of setup, `0x28fe67d` and `0x28fe6fd`, follow an event dequeue
from the movie object's `+0x48` structure and compare the event ID with 2, matching
`AvPlayerEvents::StateReady`. Setup stores movie state 3 at entry and state 2 only
on its successful exit. These are inferred guest-state meanings, not recovered
source enum names. The draw routine's entry checks the AvPlayer handle, activity,
and video-frame availability; it does not check `movie+0xe0` there.

Cleanup `0x28fe2e0` destroys and frees a non-null wrapper, then clears
`movie+0xe0` at `0x28fe59e`, before calling `sceAvPlayerClose` at `0x28fe5d3`.
These are concrete initialization and teardown sites, not an exhaustive search
for arbitrary writes or other aliases to the field.

## What remains unproven

The canary log records StateReady and StatePlay, then many movie frames. It has no
logged StateStop, demux EOF, or decoder exit before this fault. That makes a claim
of normal EOF teardown unsupported; it does not exclude entry into cleanup before
Close logging, concurrent object reuse, or corruption.

The pointer chain contains ordinary guest CPU loads/stores, not a fetch of a
GPU-produced pointer. It is compatible with a null movie field, an overwritten
draw-state slot, or an unexpected register/control-flow corruption. Without the
live field values, the static chain alone cannot distinguish those possibilities.
The host PageManager symbol in the signal-time backtrace is not evidence that a
Mach alias supplied a stale shader pointer. The separate synchronized CPU-alias
test is documented in [command-buffer-visibility.md](command-buffer-visibility.md).

For the next owner-controlled in-process diagnostic, capture the fault registers,
the draw-state address and `+0xa88` slot, the movie address and its `+0xe0`,
`+0x98`, and `+0x8` fields, plus setup's return status and cleanup entry timing.
Comparing movie and draw-state slots distinguishes an already-null movie shader
from corruption after publication. Correlating those observations with command
consumption/completion can test the common-race hypothesis; it is not established
by this audit. No repair or rendering-performance claim follows from static
analysis alone.
