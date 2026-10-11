# Movie step handoff to GXRenderThread (`BB_MOVIE_HANDOFF`): falsified and removed

> **Falsified, 2026-10-11; removed from the code.** This note is kept as the record. The
> movie/title race itself is parked: the game is playable without the attract video.
>
> **H1** (Playtest, title idle at `--fps 30`, FPS++ 30 preset, 17d,
> `out/macos_route/20261011-182501-1080p-TI30-17d-H1`):
> - The kick mode byte was 1, and the movie step ran on main.
> - The handoff did not remove the race: there were still 16 `CMD-INTERLEAVE`. In #1, two lines
>   after the first movie step, GXRenderThread is *starting a frame*
>   (`sceGnmDrawInitDefaultHardwareState200`) before main writes.
> - Meanwhile the first 600 movie steps found GX's done event already set, so 0 waited. In mode
>   1, done does not bracket GX's frames.
> - Later 6 steps waited (mean 57 ms, max 132 ms). Then done never came within 5 s (event state
>   0x5: initialized, a waiter, not signaled). The handoff's own stop ended the run. It could
>   hang the game.
>
> The H0 control at `--fps 30` (without the handoff) was not run: the race is parked, so this
> rests on H1 alone. The A, B and C title-idle arms without the handoff (earlier, uncapped) had
> the same 16 `CMD-INTERLEAVE`.
>
> What is still unknown, and not being pursued: the mode-1 kick path (whether it resets done),
> and what else starts GX's frames.

Title-screen idle crashes once the attract video (`sprj_advertise.mp4`) starts. So do some movie
crashes. The builder trace reports `CMD-INTERLEAVE`: the main thread and GXRenderThread
advance one command stream. The parser then stops on a torn packet (`Wrong PM4 type`,
`Unimplemented PM4 type 0`), or the guest faults at 0x26c01c3.

## Cause (static RE, Bloodborne 01.09; image offsets)

- **GXRenderThread's loop** (0x21b6910, object G):
  - runs the frame job in `[G+0x20]`, which builds the frame in the renderer's context;
  - clears the job;
  - sets its **done event** at G+0x68 (0x21b6a68);
  - waits for the kick event at G+0x28.

  The `[Debug] Delay Render` byte (0x55406c0) would add a 40 ms sleep before each job. It is
  off in retail and no patch sets it.
- **The kick** (0x21b6aa0) waits for done, stores the job, resets done and sets the kick
  event. Its mode byte `[[G+0x88]+0x72]` selects an asynchronous or a synchronous path. The
  wrapper 0x2199be0 takes `G = [R+0x348]`, where R is the renderer at `[0x55406c8]`.
- **The movie step** (0x1fce3b0, main thread):
  - sleeps 20 ms (the usleep call at 0x1fce3fa);
  - then draws the movie (vtable +0x70 = 0x28fe7c0) into `[[R+0x10]+8]`, GX's context.

  Nothing found orders the draw after GX's frame except that sleep. Our hypothesis is that the
  step relies on GX finishing within it; on the PS4 this is inferred, not measured. Translated by
  Rosetta, the two threads are seen writing at once:
  - At one stop, GX started its frame 4.6 µs before main wrote.
  - No placement of our submit throttle changed this. The A, B and C title-idle arms all
    showed 16 `CMD-INTERLEAVE`.

## The experiment

With `BB_MOVIE_HANDOFF=1` (src/runtime_movie_handoff.c), only that one sleep is followed by a
wait for GX's done event.
- **Done is read only under the event's mutex** (event+8, through the engine's own mutex
  imports 0x2bbfef8/0x2bbff08).
- **While it is clear, the step sleeps 250 µs and checks again.** It does not wait on the event's
  condition: every FPS++ patch preset turns the engine's timed condition wait (helper 0x2083e80,
  its call at 0x2083ec1) into a mid-function `ret`, so a timed wait through it would crash, and
  an untimed one could not be bounded. Polling under the mutex reads the same synchronized state
  and writes nothing of the guest's.
  - A 5 s deadline is checked after every check of done, so done seen only past it still
    stops. It bounds the polling; it is not a watchdog against a lock that never returns.
- **If GX already finished within the 20 ms**, done is set and nothing waits.
- **If only the main thread kicks GX** (not established, see below), no kick can come between
  the wait and the draw. The draw then follows GX's frame.
- **Matching the call site:**
  - The sleep is matched by its exact guest return address, 0x1fce3ff, in both
    `sceKernelUsleep` and `usleep`. Other sleeps are unchanged.
  - Only when `BB_HLE_TRACE` or `BB_HLE_TRACE_SUMMARY` replaced the return slot with its
    bridge does the original address come from the trace's record for that slot. A slot not
    holding the bridge is taken as is, even if a stale record names it.
- **The sleep's result**, errno and guest errno are kept.

Output (stdout):
- `Movie handoff: on` at load.
- The kick mode byte and the movie step's thread, the first time and whenever the mode
  changes.
- Every 600 movie steps: how many waited for GX, with the mean and the maximum. These
  statistics are kept per thread.

### Loud stops

- At load, after relocation and patches and before `BB_GUEST_ENTRY_TRACE` writes its
  breakpoints:
  - any code range the hook relies on differs from the analysed 01.09 image (FNV-1a per
    range, naming the range). The ranges cover the movie step, the sleep and mutex import
    stubs, GX's done set and kick wait, the kick in both modes, the kick wrapper, the event's
    set, and its wait method through unlock and return (the event type check). The FPS++-patched
    condition-wait helper 0x2083e80 is not relied on, so an FPS++ image passes;
  - the image is too small for the slots read;
  - the movie step's import slot is not the runtime's usleep, possibly wrapped by the trace;
  - `BB_MOVIE_HANDOFF` is something other than `0` or `1`.
- At a movie step:
  - R or G is null;
  - the done event is not initialized, or its wait method is not 0x207e8f0;
  - locking or unlocking the event's mutex fails;
  - GX's frame is not done within 5 s, including done seen only after the deadline (a check
    blocked on the mutex past it).

### Not established (why it is opt-in)

- **That only the main thread kicks GX.** Then no kick can come between this wait and the
  draw. The qualification run records every caller thread of the kick, its direct caller and
  the movie step, through title idle, the attract video and Continue:
  `BB_GUEST_ENTRY_TRACE=0x21b6aa0,0x1d978e0,0x1fce3b0`. The wrapper 0x2199be0 has no push-rbp
  entry, so it can't be a site. The kick records also catch its tail jump, with the original
  caller.
- **The kick's arguments** in the modes actually used.
- **That GX finishes within 20 ms on the PS4.** This is inferred, not measured.

The test is the title-idle repro with and without the hook. Success is 0 `CMD-INTERLEAVE` and
no parser stop while the attract video plays.

## Test

`tests/test_movie_handoff_macos.sh` builds the call as the game makes it:
- a fake image with `mov edi, 20000; call <stub>` at the movie step's offset;
- the sleep and mutex stubs, `jmp [slot]`;
- import slots holding a usleep built like the runtime's, not runtime_kernel.c itself;
- the engine objects the hook reads, and a wait method that aborts if called.

It runs untraced, traced (`BB_HLE_TRACE`) and summary-only (`BB_HLE_TRACE_SUMMARY`). It checks:
- polling until done is seen under the mutex;
- no wait when GX is already done;
- no wait for a sleep from another call site;
- errno and guest errno kept;
- two movie steps at once;
- a stale trace record for the same slot is ignored;
- each loud stop, in a forked child, matched by its message.

The analysed game code is not in the repository, so the image check is tested only against an
image that differs.
