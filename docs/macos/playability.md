# Save, audio and controller qualification

Status: 2026-10-11. No new game run was used for this unit. Physical controller
qualification and audible game quality require the user; scripted pad injection
and CoreAudio port activity cannot establish either.

## Saves

The inspected Playtest manifest was
`20261010-224225-1080p-PAR-ep2-entry`. Its `BB_USER_DIR` resolves beneath the
Playtest worktree, on `/dev/disk3s5`, local APFS. It is not beneath Space or on
JuiceFS. Each route uses a separate user directory. Arbitrary user overrides
are not qualified by that observation.

On Apple, shim-owned `param.bin` and `icon0.png` are now replaced by writing a
same-directory temporary file, checking all writes, fsync and close, then rename.
A failed preparation leaves the previous destination intact. SaveDataMemory
writes flush and fsync before returning success; a failed setup cannot publish a
candidate path paired with an old size. Icon capacity and SaveDataMemory size/
offset overflow are validated. Partial metadata updates reject unreadable metadata.
The native paths only share strict validation/error fixes.

The existing wired savedata fixture execs fresh writers/readers and proves all
four payloads (guest file, metadata, icon, SaveDataMemory) survive normal exit
and SIGKILL immediately after successful commit. Its guest file explicitly calls
sceKernelFsync before close. Injected metadata sync/rename errors preserve the
previous metadata. Failed user-2 setup leaves the successful user-1 setup usable,
without subsequent writes reaching the failed candidate. This is process-crash
coverage, not a power-loss guarantee, whole-save transaction, or backup rotation.
No F_FULLFSYNC was added. No additional commit API is imported by this eboot.

Remaining audit findings: mount capacity is not enforced; search capacity is
hardcoded; there is no interprocess save lock or automatic backup. The shim's
metadata is `param.bin`, not a PS4-format param.sfo. Existing sound-hack startup
mutation of userdata0010 is unchanged. These are not silently claimed fixed by
the atomic metadata unit.

Playtest acceptance requested: autosaved item/lamp change, wait for the save
indicator, normal quit, preserve exact savedata and hashes/sizes, fresh process
Continue, verify inventory/location. That requires playable world entry.

## Audio

New Apple counters are cumulative input-empty observations and successfully
inserted silence frames, with a separate first-five-seconds warmup per port.
The first post-warmup incident prints AUDIO HEALTH FAILURE; the final report
retains totals, including after port closure. Playback policy/cadence is unchanged.
Backend failures are counted and retain their existing loud stop. No intentional
input-drop path was found; no fabricated hardware-drop counter was added.

[SDL_GetAudioStreamQueued](https://wiki.libsdl.org/SDL3/SDL_GetAudioStreamQueued)
reports unconsumed input bytes. These observations are not hardware underrun
counts. Device format and hardware-feed quantum are printed on open; the quantum
is not total output latency. The signed fixture passed on MacBook Pro Speakers:
48 kHz, stereo S16, 1024-frame quantum (21.333 ms), 96 submitted buffers,
two warmup empty observations and 512 inserted silence frames, zero backend errors.
That half-second run cannot qualify post-warmup game playback. Deterministic
controls separately verify the warmup boundary and non-resetting report counters.

## Controller imports and consumers

The read-only SELF symbol/relocation scan covers CUSA03173 eboot and all seven
sce_module PRXs. Only the eboot imports libScePad: Init, Close, Open, ReadState,
GetControllerInformation, ResetOrientation, SetAngularVelocityDeadbandState,
SetTiltCorrectionState and SetVibration. No PRX imports libScePad. There are no
SetLightBar, SetMotionSensorState or IsMotion imports. Dynamic symbol lookup is
not ruled out by this static scan.

Absence of a motion-enable import does NOT mean motion is unused. ReadState at
0x2690168 writes the input object's PadData beginning at +0x110. Instructions
0x2690444..0x2690627 consume its acceleration (+0x1c/+0x20/+0x24) and angular
velocity (+0x28/+0x2c/+0x30); 0x2690751 copies its quaternion (+0x0c). Acceleration
is clamped at +/-3; angular velocity at approximately +/-6.5345. A second
ReadState call is at 0x2691948. Only offsets/findings are retained here, no game bytes.

Motion is a diagnostic implementation, OFF by default. Set
`BB_PAD_MOTION=diagnostic` only for sensor qualification. Production motion
replacement awaits the orchestrator's policy decision and physical qualification;
the save/audio/controller-error units are independently mergeable.

Apple diagnostic motion uses SDL sensor events with separate monotonic arrival and device
sample timestamps. Acceleration is converted from m/s² to g; angular velocity
remains rad/s, with SDL's controller axes unchanged. Enabled gyro samples update
a normalized quaternion by constant-rate rotation over each sample interval.
Reset reanchors identity without discarding sensor timestamp watermarks; disable
suppresses samples, and re-enable establishes a new SDL arrival epoch. Events
with pre-epoch arrival timestamps or old accepted sensor timestamps are rejected.
SDL does not expose a synchronous hardware clock fence: a never-before-observed
old sample with both a new arrival timestamp and a newer device timestamp cannot
be identified as pre-reset from these fields alone. Repeated/backwards gyro
timestamps do not integrate twice. Intervals exceeding 250 ms are not integrated;
aged vectors are suppressed with a named diagnostic. Sensor-less devices retain
button support and report their absent sensor capabilities.

This is a gyro-integrated orientation estimate, not a qualified reproduction of
PS4 sensor fusion, drift correction or filter tuning. Requests to enable PS4
angular deadband or tilt correction remain explicitly reported as unqualified;
no invented filter is silently applied. Device axes, gravity scale, drift and
reconnect behavior require physical DS4/DualSense checks before claiming motion
parity. The implementation adds work per sensor event and a small locked snapshot
per pad read, not per draw. SDL calls are made outside the sensor-cache lock.

References:
- [SDL sensor units and axes](https://wiki.libsdl.org/SDL3/SDL_SensorType).
- [SDL sensor events](https://wiki.libsdl.org/SDL3/SDL_GamepadSensorEvent).
- [OpenOrbis pad structure](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/blob/master/include/orbis/_types/pad.h).
- [Upstream shadPS4 gyro integration](https://github.com/shadps4-emu/shadPS4/blob/main/src/input/controller.cpp).
  This is an emulator precedent, not console ground truth. Its acceleration scale
  is approximately 0.098; this unit uses division by SDL_STANDARD_GRAVITY instead.

The virtual SDL fixture exercises actual sensor events, independent acceleration
axes, a 90-degree gyro rotation, old timestamps, reset, disable and stale samples.
It also covers touch coordinates/clicks, unequal large/small rumble motors and
injected initialization/rumble failures. Apple init/enumeration/open/rumble errors
are named and cumulatively reported; the existing guest return policy is retained.
No lightbar implementation was added without a game import. DualSense/DS4/Xbox
USB/Bluetooth transport, physical rumble and IME through real buttons remain
unverified until hardware is connected.
