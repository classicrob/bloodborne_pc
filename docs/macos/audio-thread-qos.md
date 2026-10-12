# Audio producer scheduling: source audit and proposed A/B

## Finding

The runtime does not create a dedicated audio submission worker.
`runtime_audio.c::output_port` converts, paces and submits on the guest caller.
Its header identifies FMOD's output thread and its dependency on the mixer.
The DREAM-3 log creates `FMOD AudioOut thread` with guest priority 256,
`FMOD mixer thread` with 257, and `GXRenderThread` with 700. These numbers
are guest requests, not measured host priorities.

`runtime_thread.c::create` copies the guest attributes but applies only host
stack settings before `pthread_create`. `host_start` attaches TLS and names
the thread; it applies neither QoS nor a Mach time-constraint policy. Later
guest priority setters update the stored field only. Therefore the guest's
requested audio priority distinction is lost. No current log reports the
actual requested/effective host QoS; do not substitute a guessed inherited
class for that missing measurement.

The GPU command thread (`Liverpool::Process`) and stage-B recorder
(`DrawPipe::Run`) likewise name themselves without a priority/QoS boost.
Preparation workers explicitly use `BbThreads::MakeBackground`, which
requests `QOS_CLASS_BACKGROUND` on Apple. The common Mach realtime helper
exists but these audio, parser and draw-recorder paths do not call it.

There is a second constraint: `heavy.py::polite` launches ordinary playtest
jobs at nice 5 under `taskpolicy -c utility`. The installed `taskpolicy(8)`
manual describes this as a process QoS clamp inherited by children. Merely
requesting user-interactive on a pthread is not evidence of escaping that
clamp. GPU-exclusive timing jobs are exempt in the existing launcher. The
DREAM-3 manifest has an empty `heavy` object and no per-thread QoS record,
so launch-policy attribution for that particular run still needs the job
history, rather than being inferred from its audio statistics alone.

## Opt-in experiment

The Apple-only, strictly parsed, opt-in `BB_AUDIO_QOS=interactive` mode
(`inherit`/unset preserves behavior) is implemented. In `host_start`, before entering guest
code, it applies `pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0)`
to the exact `FMOD AudioOut thread` and `FMOD mixer thread` identities. This
is a named FMOD experiment, not a complete PS4 priority mapping. Raising only
the output thread could leave its mixer starved. Do not boost FMOD file or
stream workers or all guest threads in this first unit.

It logs the two selected thread identities, guest priorities, requested host QoS
before/after once, stopping on API errors. Querying pthread QoS records the requested
class, not proof of the effective scheduling class under process clamps.
Keep errors loud when the explicit experiment cannot be applied. In stats
mode, it logs the actual AudioOut caller once per port too, to confirm this
name-based selection targets the producer. The startup hook preserves host and guest errno.

Use the documented [pthread QoS API](https://developer.apple.com/library/archive/documentation/Performance/Conceptual/power_efficiency_guidelines_osx/PrioritizeWorkAtTheTaskLevel.html).
A Mach realtime policy needs a qualified computation budget and bounded
callback work; arbitrary budgets around guest mixing, mutexes and SDL calls
would not be justified by these logs. [Audio workgroups](https://developer.apple.com/documentation/audiotoolbox/understanding-audio-workgroups)
are a subsequent device-integration option, not an automatic consequence of
raising QoS.

Do not change the pacing in this unit. It currently uses absolute monotonic
deadlines at frames/48000, with input-queue feedback of plus/minus period/32
to compensate device drift, and restarts after a stall exceeding eight
periods. It is not directly driven by a CoreAudio sample-position clock.
Keep conversion, buffer size, silence insertion, deadlines and feedback
identical in both arms; no free-running or burst-drain shortcut.

## Qualification and A/B

1. Fixture first: both selected identities request interactive exactly once,
   other names and default mode do not; invalid mode/API failure are loud;
   errno is preserved. Run the existing audio cadence/null-output fixtures
   unchanged. Run actual touched tests and the separate test-dry gate.
2. On one pinned build and warm Dream fixture, use real CoreAudio and
   `BB_AUDIO_STATS=1`, with identical 60-second warmup and at least three
   minutes of still/walk route per arm. A is `BB_AUDIO_QOS=inherit`, B is
   `BB_AUDIO_QOS=interactive`; repeat in reversed order if the difference
   is material. No CPU profiler or null output.
3. Have Build schedule both through the existing GPU-exclusive timing lane
   so neither has the utility clamp; keep host load and launch policy equal.
   Do not disable the user's polite policy globally. An additional paired
   clamped comparison can measure the launcher effect, separately from QoS.
4. Retain every stats line: count distribution, maximum producer gaps, empty
   observations and silence counters, plus game frame time and host load.
   Report elapsed window time if instrumented; the existing "/5s" windows
   are rounded by call arrival. Separate boot and steady gameplay. A benefit
   means fewer/shorter producer gaps and empty observations without broken
   cadence or a material game regression. Listening remains required.

No launcher policy or GPU-thread priority is changed. No game A/B has run.
Actual requested/effective QoS of the GPU parser and recorder in a game has
not been measured under Rosetta: absence of a source override establishes
inheritance, not its numeric effective class. An ordinary play_route game
uses the playtest policy described above; a perf run taking the GPU
exclusively is launched without that clamp/nice adjustment by heavy.sh.
Parent-process policy may still be inherited. A future in-process diagnostic
at those threads' entry points must record requested QoS and effective Mach
policy/priority plus the launch policy before claiming their actual classes.
This is a separate perf lever for PM4/Renderer.
