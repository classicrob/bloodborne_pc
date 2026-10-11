# Movie step handoff to GXRenderThread (`BB_MOVIE_HANDOFF=1`, experiment)

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

  Nothing orders the draw after GX's frame except that sleep. On the PS4, GX finishes within
  it. Translated by Rosetta, GX often does not:
  - At one stop, GX started its frame 4.6 µs before main wrote.
  - No placement of our submit throttle changed this. The A, B and C title-idle arms all
    showed 16 `CMD-INTERLEAVE`.

## The experiment

With `BB_MOVIE_HANDOFF=1` (src/runtime_movie_handoff.c), only that one sleep is followed by a
wait for GX's done event.
- **The wait** uses the engine's wait method (event vtable +0x18 = 0x207e8f0: the event's
  mutex and condition), in 100 ms slices.
- **If GX finished within the 20 ms** (the PS4's timing), the event is already set and the
  wait returns at once.
- **The draw then follows GX's frame**, as it does on the PS4.
- **Matching the call site:**
  - The sleep is matched by its exact guest return address, 0x1fce3ff, in both
    `sceKernelUsleep` and `usleep`. Other sleeps are unchanged.
  - When `BB_HLE_TRACE` or `BB_HLE_TRACE_SUMMARY` replaced the return slot with its bridge,
    the original address comes from the trace's record for that slot.
- **The sleep's result**, errno and guest errno are kept.

Output (stdout):
- `Movie handoff: on` at load.
- The kick mode byte and the movie step's thread, the first time and whenever the mode
  changes.
- Every 600 movie steps: how many waited for GX, with the mean and the maximum.

### Loud stops

- At load, after relocation and patches and before `BB_GUEST_ENTRY_TRACE` writes its
  breakpoints:
  - any code range the hook relies on differs from the analysed 01.09 image (FNV-1a per
    range, naming the range);
  - the movie step's import slot is not the runtime's usleep, possibly wrapped by the trace;
  - `BB_MOVIE_HANDOFF` is something other than `0` or `1`.
- At a movie step:
  - R or G is null;
  - the done event is not initialized, or its wait method is not 0x207e8f0;
  - a wait fails;
  - GX's frame is not done within 5 s.

### Not established (why it is opt-in)

- **That only the main thread kicks GX.** Then no kick can come between this wait and the
  draw. The qualification run records every caller thread of the kick, the wrapper and the
  movie step: `BB_GUEST_ENTRY_TRACE=0x21b6aa0,0x2199be0,0x1fce3b0`, through title idle, the
  attract video and Continue.
- **The kick's arguments** in the modes actually used.
- **That GX finishes within 20 ms on the PS4.** This is inferred, not measured.

The test is the title-idle repro with and without the hook. Success is 0 `CMD-INTERLEAVE` and
no parser stop while the attract video plays.

## Test

`tests/test_movie_handoff_macos.sh` builds the call as the game makes it:
- a fake image with `mov edi, 20000; call <stub>` at the movie step's offset;
- the stub `jmp [slot]`;
- an import slot holding a usleep built like the runtime's;
- the engine objects the hook reads.

It runs untraced, traced (`BB_HLE_TRACE`) and summary-only (`BB_HLE_TRACE_SUMMARY`). It checks:
- the wait in slices until done;
- one immediate wait call when GX is already done;
- no wait for a sleep from another call site;
- errno and guest errno kept;
- each loud stop, in a forked child.

The analysed game code is not in the repository, so the image check is tested only against an
image that differs.
