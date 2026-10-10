# PM4 memory writes vs. the command parser

## The race (all platforms)

bbport splits the graphics command processor in two (`vk_draw_pipe.h`):

- **Stage A** (GPU command thread) parses PM4 straight from guest memory: top-level DCBs,
  nested indirect buffers, and the `COND_EXEC` predicate it reads to decide whether to skip the
  next N dwords.
- **Stage B** (draw recording thread) records the draws. With `PipelinedMemoryWrites` (bit 42,
  on by default) and `PipelinedTasks` it also performs the stream's memory writes, in stream order
  but later: `WRITE_DATA` as a CPU copy, EOP/EOS fences, and `DMA_DATA` as GPU copies and fills.

The PS4 command processor executes an ME write before it fetches the packets that follow it. Here
stage A can read command memory, or a `COND_EXEC` predicate, *before* stage B has performed a write
from earlier in the stream. A predicate read stale makes the parser execute a region the game
meant to skip, and skipped regions can hold placeholder or unfinished packets. The parser then
loses packet alignment.

Linux and Windows have the same split and the same exposure; there the parser rarely runs far
enough ahead of stage B to lose the race. Under Rosetta, pipeline compiles keep stage A busy in
bursts while the guest keeps submitting, and it loses deterministically.

## Evidence (Bloodborne v1.09, M4 Max, 2026-10-10, Opus-Playtest)

| Configuration | Movie start | After name entry / "Finalise contract" |
|---|---|---|
| default (bit 42 on), 3 runs | `NextPacket: packet length exceeds remaining submission size` 3/2 every run | 18/10, then fatal `Wrong PM4 type 1` |
| bit 42 off (WRITE_DATA/DMA_DATA in order on stage A) | clean | clean, past the contract (stopped later by geometry shaders) |
| all pipelining off | clean | not reached (unrelated guest fault in the movie) |
| private canary (extra work on stage A per buffer, so stage B catches up) | clean | not reached |
| `BB_COPY_GPU_BUFFERS=1` (copy at submit: freezes pre-write content) | — | died earlier, out-of-order flip IRQ |

A submit-time snapshot of command memory was considered and rejected: it freezes exactly the
pre-write words.

## Fix (Apple; `Liverpool::SyncCommandMemory`, `NoteCommandMemoryWrite`)

Stage A lets earlier stream writes land before it reads guest memory that they may have changed:

1. On entering any command buffer (top-level or nested IB): if a write handed to stage B and not
   yet performed overlaps it, drain stage B (and the fences it deferred).
2. Before reading a `COND_EXEC` predicate: the same.
3. On handing over a `WRITE_DATA`/`DMA_DATA` whose destination overlaps the unparsed rest of a
   buffer being parsed: drain right away.
4. `DMA_DATA` runs on the GPU, so an overlap with a DMA destination the GPU may not have written
   yet waits for the GPU (`Rasterizer::Finish`). This applies with or without pipelining: a DMA
   into command memory was read before the GPU copy ran on every configuration. DMA destinations
   are remembered until the flush holding them has completed.

The cost is zero unless a write targets command memory or a predicate. Each sync is logged (first
eight, then every 1024th), and every 600 frames a summary gives drain and GPU-wait counts and
times. Writes from compute queues and constant-engine buffers are not covered: the PS4 does not
order them against the graphics parser either.

## Upstream

The same fix applies to Linux/Windows bbport (and to shadPS4 if it ever parses ahead of memory
writes). It is Apple-only for now because only macOS could be tested here.
