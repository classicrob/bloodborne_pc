# Parse split: where the GPU-side threads' time goes (macOS, always on)

R0 (docs/macos/throttle-relaxation.md) showed that the whole sceGnmSubmitDone-to-GpuIdle wait is
"parse and queue". The existing stats show that the GPU command thread (stage A) is on the CPU
for only 9-23 ms of a 100-200 ms frame. It spends most of the rest waiting for the draw recording
thread (stage B). These counters split each of the three threads' wall time.

## How it counts (gpu/shim/bbport_parse_split.h)

Each thread's time is partitioned:
- `BbSplit::Mark(bucket)` charges the interval since the previous mark to the bucket open then.
- `BbSplit::Scope{kind}` (RAII) charges its own time to its kind and takes it out of the
  enclosing bucket. Scopes nest, and each is charged without its inner scopes. Its time is also
  kept per bucket, so you can see, for example, deferred-signal waits by stage A packet class.
- `BbSplit::Section{account, bucket}` opens a bucket for its lifetime on that account's thread
  only, then reopens the previous one. This is for code that both stages run.

Mechanics:
- Counts use `__rdtsc`; there are no clock calls and no locked instructions. Each thread is the
  only writer of its account, and the reporter reads it with relaxed loads.
- A thread without an account records nothing. For example, the texture helper and the
  preparation workers are not counted.
- The macros `BB_SPLIT_SCOPE`, `BB_SPLIT_SECTION` and `BB_SPLIT_MARK_ON` compile to nothing off
  Apple.

## Accounts and buckets

**Stage A, the GPU command thread** (liverpool.cpp):
- one bucket per graphics packet class: registers, draws, indirect draws, dispatches, labels,
  WAIT_REG_MEM, memory packets, INDIRECT_BUFFER, other;
- constant engine and compute queue packets;
- host commands, the scheduler loop, frame end and GpuIdle;
- waiting for submissions.

**Stage B, the draw recording thread** (vk_draw_pipe.h, vk_rasterizer.cpp):
- no packet, decode, in-order tasks, special draws, dispatches, indirect draws;
- for direct draws: setup, render state, bind, begin rendering, record;
- bind buffers and bind textures, wherever they run on stage B;
- after packet.

**Vulkan recording thread** (vk_scheduler.cpp): idle, executing chunks.

**Scopes:**
- stage B drains, by reason: DUMP_CONST_RAM, idle, submit/flush, other packets;
- draw pipe full;
- deferred signals (`WaitDeferredSignals`, labels at GPU completion);
- recorder signals, recorder sync, host copies, GPU ticks, presentation label;
- queue submit.

Opus-Renderer adds the stage B recording internals and the recorder's per-command-type encoding,
as further scope kinds.

## Output

One block every 600 frame ends, printed from stage A:

```
Parse split, last 600 frame ends: <ms> ms per frame, <n> draws per frame; ms per frame by thread (accounted / wall)
  stage A (GPU command thread, <accounted %>): <bucket ms, ...>; in scopes: <kind ms, ...>
    deferred-signal waits during: <stage A bucket ms, ...>
    stage B drains at other packets during: <stage A bucket ms, ...>
  stage B (draw recording thread, <%>): ...
  Vulkan recording thread (<%>): ...
```

How to read it:
- The accounted percentage should be close to 100%. Only the interval still open at the report
  is missing.
- Waits overlap across threads. For example, stage A waits in a drain while stage B waits on the
  recorder. Read each thread's line on its own.
