# Audio service CPU investigation

## Status

Source audit, 2026-10-11. During the quiet performance window, Build reported
approximately 200% CPU in coreaudiod and 100% in audioanalyticsd. Those observations
do not yet establish that bbport caused either load. No audio configuration was
changed and no measurement job was started during this audit.

The source does not support a tiny CoreAudio quantum or repeated device opening
as the explanation. The first useful experiment is a matched idle / game audio /
game timer-sink / idle comparison after the quiet window.

## Runtime and observed configuration

`src/runtime_audio.c` initializes SDL audio once and opens one logical stream per
guest AudioOut port. Each stream is resumed once and destroyed on port close.
Output calls apply volume/channel order, enqueue PCM, and pace the guest. They do
not reopen a device, change its format, or set a resampling ratio. Cadence correction
changes the next guest deadline, not the stream's sample rate.

The full log from Playtest's
`20261011-003704-1080p-P0-41bd332-fixture` has exactly two audio port opens and two
device-format reports:

| Property | Observed value |
| --- | --- |
| Backend / device | coreaudio / MacBook Pro Speakers |
| Guest ports | 0 (main), 8 (BGM) |
| Input and opened SDL format | 48,000 Hz, eight channels, float32 |
| Guest write size | 256 frames, 5.333 ms |
| Opened SDL device quantum | 1,024 frames, 21.333 ms |

Only port 0 produces five-second stats in that log. This does not prove port 8
never receives data; it does rule out a long-running second stream of stats at
the same cadence. Most port-0 windows contain about 938 buffers, versus 937
expected. Some gaps reach 82 ms under load. The inserted-silence warning describes
an empty software input queue, not a measured hardware underrun or daemon CPU
cost. `BB_AUDIO_STATS` adds a queue query per input buffer and a report roughly
every five seconds; it does not change the device configuration.

## SDL implementation cross-check

The installed dependency and packaging recipe identify SDL 3.4.18. The audit used
its official [audio core source](https://github.com/libsdl-org/SDL/blob/release-3.4.18/src/audio/SDL_audio.c)
and [CoreAudio backend](https://github.com/libsdl-org/SDL/blob/release-3.4.18/src/audio/coreaudio/SDL_coreaudio.m).
Local inspection copies are in `out/audio-source-audit/`.

`OpenPhysicalAudioDevice` returns immediately when already opened: two logical
ports share one physical output in this process. Two streams select SDL's float
mixing path rather than its single-stream copy path. This is in-process work.
The backend creates one AudioQueue output, uses the requested PCM format, and
reenqueues buffers from its callback. At the observed quantum it allocates three
buffers; their combined capacity is 64 ms, not a measurement of end-to-end latency.
Its service thread waits in a CFRunLoop. No repeated output-loop reconfiguration
was found.

`SDL_GetAudioDeviceFormat` reports the opened SDL format. Device discovery queries
HAL channel count/rate, but opening can raise the requested channel count/rate.
Consequently the eight-channel report does not establish the speakers' physical
format or exclude downstream CoreAudio conversion. Eight-channel output uses a
WAVE 7.1 layout. No source reference to audioanalyticsd was found in our audio path.

## Ranked questions, not established causes

1. **Is the service load caused by this game at all?** Other audio clients and
   analytics activity share those daemons. A before/after CPU delta is needed.
2. **Does the eight-channel client format cause costly downstream processing?**
   It is the strongest configuration lead remaining in source. Read the actual
   HAL output rate/channel configuration before attributing cost to conversion.
3. **Does the second logical stream matter?** It forces in-process SDL mixing,
   but does not create another physical AudioQueue in this process. This alone
   is not an explanation for two cores in coreaudiod.
4. **Is starvation a cause or consequence of contention?** Existing logs show
   delayed guest audio input. Correlate queue reports with CPU measurements; do
   not infer service CPU from an inserted-silence event.

Multiple game processes each have their own physical AudioQueue. Record the
number of running processes when comparing results.

## Post-window measurement plan

Coordinate one game slot with Playtest/Build; use one pinned artifact and the
same clinic-start scene. Keep output device, volume, other audio clients, display
resolution and profiler configuration fixed. Do not restart system daemons.

1. Record 30 seconds idle, with no game process.
2. Run the normal audio path with `BB_AUDIO_STATS=1`; record at least 30 steady
   seconds in the clinic after loading, plus the port/device log and health totals.
3. Repeat from the same fixture with `BB_AUDIO=none BB_AUDIO_STATS=1`. The existing
   timer sink avoids SDL audio initialization while retaining guest PCM work and
   nominal pacing. Queue-based cadence correction is absent, so this is an
   isolation control, not an identical timing workload or a proposed fix.
4. Record idle again after the game exits. Repeat paired runs if the daemon load
   changes independently of game audio.

Use cumulative process CPU-time deltas divided by elapsed wall time for the
game, coreaudiod and audioanalyticsd; 100% means one CPU core. Record PIDs, restarts,
host load and other running game processes. Avoid treating a single `ps %cpu`
snapshot as the interval measurement. Our in-process profiler measures only
bb-probe, so it cannot explain the daemons' internal hotspots.

If normal audio alone reproduces the daemon load, use a small standalone fixture
to compare eight-channel and stereo output, one and two logical streams, and
the same quantum. A stereo production change would require a correct, tested
downmix; dropping channels or disabling sound is not a fix. No such runtime
change is justified by this source audit alone.
