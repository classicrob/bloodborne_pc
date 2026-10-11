# Clock-paced null audio output

Set `BB_AUDIO_OUTPUT=null` for automated correctness runs that must not depend on the host's
audio device or CoreAudio service. On the first audio-port open the runtime prints:

```
Audio: null output (BB_AUDIO_OUTPUT=null), no sound
```

The selection is latched at that first open. It bypasses SDL audio initialization and device
opening. No automatic selection occurs: unset/empty keeps the existing device path, including
the loud macOS stop on device initialization or stream-open failure. Other nonempty values
stop. The pre-existing explicit `BB_AUDIO=none` timer selection remains supported; the new
explicit null selection takes precedence. The small selection change is shared across platforms;
execution qualification is on macOS.

The sink shares the normal AudioOut conversion, volume, optional PCM dump, return values,
last-output timestamps and pacing code. Each port retains its requested buffer length at
48000 Hz. The first output starts the cadence; subsequent outputs wait for the corresponding
absolute monotonic deadline. Batched ports wait once per batch, as in the device path. EINTR
retries keep the deadline, and a stall longer than eight periods resets the cadence, as before.
There is no independent device clock, so the device path's queue-feedback drift correction is
not applied. This is real-time nominal pacing, not emulation of CoreAudio's hardware clock,
queue depth, or output latency.

`BB_AUDIO_STATS=1` still reports buffer cadence and maximum gaps; null output explicitly marks
device queue health unavailable. It does not invent hardware underrun measurements. It cannot
qualify audible output, device layout or latency, and it does not remove CPU load from an
already unhealthy system audio daemon. Performance runs still require a healthy host baseline.

`tests/test_audio_macos.sh` checks null mode with a deliberately unavailable SDL driver: no SDL
audio subsystem is initialized, 256/512/2048-frame buffers remain paced, batch pacing, volume,
timestamps, interrupted sleeps and close/reopen work. Separate controls keep missing-driver and
invalid-selection failures loud. Real-device testing remains separately opt-in.
