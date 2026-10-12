# Boot audio health warning: meaning and evidence

The warning is a **nonfatal input-queue observation**, not an SDL/CoreAudio
error or proof that sound stopped. It is not qualified as a harmless warmup
artifact either: the shim really inserted 256 silence frames at that point.
At 48 kHz that is 5.333 ms. User audibility remains unverified.

## Since when and what happens

`d974a3584720181f5cc0aa2205f548ae71abdff4`, authored October 10, 2026 at
23:53:18 -0400, added the counters and `AUDIO HEALTH FAILURE` line. The commit
did not introduce the low-queue silence-insertion policy. This observation
already appeared in the PREVIEW-2/41bd332 title runs, before Dream fixtures.

In `src/runtime_audio.c`, `record_queue_health` starts its warmup clock at
the first queue observation for each port. The boundary is five elapsed
monotonic seconds, not five seconds of uninterrupted successful audio and
not entry into gameplay. A loading stall counts toward it. After that, either
an empty input queue or inserted silence triggers the warning. It prints
only once per port lifetime: one printed line does **not** mean one occurrence.
The cumulative counters continue to increase. The boundary itself is covered
by existing injected-clock tests; nothing here suggests arithmetic turns a
nonempty queue into an empty one.

`output_port` reads the SDL input queue after pacing and before appending the
new guest buffer. If it holds less than one guest buffer, the existing policy
inserts up to one buffer of silence, records health, and then submits the real
converted samples. The warning neither closes the stream nor changes the
return policy. Failed queue queries/submissions use the separate loud
`STOP: macOS audio ... failed` path.

SDL specifies that [GetAudioStreamQueued](https://wiki.libsdl.org/SDL3/SDL_GetAudioStreamQueued)
counts input bytes, not output-ready data or hardware occupancy. Thus zero
does not establish an audible gap; the device can already have consumed a
quantum into its playback buffer. Conversely, successfully inserting silence
is a real policy action, so it must not be dismissed solely because the log
appears during boot.

## Existing run evidence

Playtest's PREVIEW-24 `20261011-231044-1080p-DREAM-1` and
`20261011-231756-1080p-DREAM-2-reload` both opened CoreAudio/MacBook Pro Speakers
at 48 kHz, eight channels, 1024-frame quantum (21.333 ms), with 256-frame
guest buffers (5.333 ms), on ports 0 and 8. Their warnings are at lines 430
and 419 respectively; neither manifest enabled `BB_AUDIO_STATS`. DREAM-1's
exit record is zero. These logs establish device opening and continued game
execution, **not sustained audio submission or audible playback after the
warning**. They contain no end-of-run cumulative audio report.

An earlier stats-enabled run does establish continued submission:
`20261011-003704-1080p-P0-41bd332-fixture/game.log` prints the warning at line
576, then many later port-0 windows at 938 buffers/5 s (expected 937), including
zero-starvation windows at lines 1394 onward. The late window at 8861 has 928
buffers, a 73.78 ms maximum producer gap and one empty-queue observation;
the next has 938 buffers and zero empty observations. This is recoverable
queue starvation, not a latched audio shutdown. Those gaps were measured in
an earlier run and cannot be assigned to the new Dream warning.

All run names above are under Playtest worktree
`thr_8wif2jice3-1/bloodborne_pc/out/macos_route/`. No new game was launched.

## Next qualification and diagnostic wording

Playtest has been asked to add `BB_AUDIO_STATS=1` to a future ordinary
correctness run, retaining windows from boot through gameplay; no extra slot
was requested. That establishes submission continuity and repeated empty
observations. The user's listening check is still needed for audibility,
crackle/gaps and channel balance.

Recommended diagnostic-only follow-up: name the line an input-queue/silence
observation rather than a device “FAILURE”, print actual elapsed port time
and cumulative counts, and retain loud backend errors. A five-second gate
does not classify cold-load stalls or device-quantum phase; extending it or
suppressing counters would hide evidence. Do not change buffering, silence
insertion or pacing until a device-output measurement supports the change.
