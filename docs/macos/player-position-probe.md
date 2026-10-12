# Live player-position probe: feasibility

Status: source-qualified candidate for a read-only, opt-in navigation observer.
The publisher is implemented below; game observation and coordinate/yaw calibration remain pending.
Astra-2 owns the runtime observer; Playtest owns the route controller.

## Source and address spaces

The useful prior art is bbhost's
[`world_player_position` and `world_player_block`](https://github.com/droogie/bbhost/blob/566184a74f071166299bb656c4af479e5bbe2771/src/engine/world_chr.cpp),
with its [WorldChrMan layout](https://github.com/droogie/bbhost/blob/566184a74f071166299bb656c4af479e5bbe2771/include/bbhost/engine/sprj/world_chr_man.hpp).
Those are leads, checked below against our local vanilla 1.09 extracted image.
Do not transplant its literal `0x593e878` into this runtime: that source subtracts
its preferred `0x400000` slide when locating the singleton. Our image-relative
offset is **0x553e878**, so its guest address is **0x80553e878** when the image is
mapped at `0x800000000`. The header's function RVAs already agree with our image
relative function offsets.

The proposed chain (each `read64` follows a guest pointer) is:

```text
world   = read64(image_base + 0x553e878)
player  = read64(world + 0x60)
modules = read64(player + 0x3b0)
physics = read64(modules + 0x68)
XYZ     = read_f32x3(physics + 0x1e0)
yaw     = read_f32(physics + 0x1d4)
map_raw = read_u32(player + 0x3f8)
```

Yaw is the character's Y rotation, in radians. It is not camera heading.
The zero direction and sign relative to left/right stick movement still need
qualification. Preserve the packed map word in the first observer; do not
silently interpret its bytes as the save editor's map selector.

## Local executable checks

All PCs in this table are image-relative offsets, not guest addresses.

| Evidence | What the local bytes establish |
| --- | --- |
| `0x18db3da` and `0x18db4dc` in local-player creation `0x18db3c0` | Load the singleton through `0x553e878`; install the newly constructed player at singleton `+0x60`. The allocation is `0x570` bytes. |
| `0x18be0d6..0x18be0f3` in character update `0x18be0a0` | Follow actor `+0x3b0`, module table `+0x68`, and read rotation at physics `+0x1d0`. |
| `0x18be135..0x18be225` | Read position at physics `+0x1e0` and construct the character transform from position and rotation. This is an update consumer, not the save serializer. |
| `0x1a4a3d0..0x1a4a45e` | Position/rotation setter copies position to `+0x1e0` and `+0x1f0`, and rotation to `+0x1d0`. Rotation reduction uses `1/(2*pi)` at `0x490b520` and `2*pi` at `0x490b560`, supporting radians. |
| `0x1a4c72c` | The physics code reads the second rotation component, `+0x1d4`, separately. The bbhost observer identifies this as yaw. |
| `0x18f17a9`, `0x18f833e..0x18f835b`, `0x18f83e0` | Player construction initializes `+0x3f8` to `0xffffffff`; position-history sampling passes this word alongside the controller; a getter returns it. The bbhost observer identifies it as the current packed block/map. |

The saved XYZ is different storage: the serializer `0x156aa50` writes the block
at `S+0x15b0`, where `S = read64(image_base+0x5556678)`. That is useful for a
save/reload cross-check, but it is not the live transform source proposed here.

The local audit captures are under `out/player-position-audit/` (untracked),
including the pinned upstream sources and disassembly. No executable bytes or
upstream implementation are added to the repository by this note.

## Publisher contract and qualification

A small Apple-only observer is feasible without a debugger, process suspension,
guest writes, or calling guest methods. Proposed env: `BB_PLAYER_POS_FILE`.
Keep it off by default; reject an unsupported image/code guard when requested.
Use non-faulting self reads, check every pointer/read, reject non-finite numbers,
and reread the pointer chain/map around the transform read. A changed identity
makes the observation invalid. These checks avoid following a torn chain, but
are not proof that a concurrently changing guest object is a coherent snapshot
or that a readable player is currently controllable.

A first qualification build should publish observations only; `goto` must not
be enabled merely because the pointer is non-null. Loading/death/menu gating
and sampling at an appropriate guest update/input boundary remain design work.
A detached 100 ms reader cannot promise either object lifetime or simulation
progress. Prefer a sample associated with a known game-thread input/update
boundary, and rate-limit publication to about 100 ms, without doing filesystem
I/O from a signal handler or holding the pad lock across it. If this boundary
cannot be qualified, label the result an asynchronous observation and require
Playtest to qualify navigation explicitly.

Use an atomically replaced, versioned JSON record rather than five bare numbers:

- process identity/run token, sequence, monotonic timestamp, and sample age;
- readable/valid status with a reason on null, failed read, changed chain or map;
- player identity, XYZ, `player_yaw_rad`, and `map_raw`;
- a separate simulation/input observation counter when available.

New file publication alone is not evidence that the game advanced. The route
tool must neutralize input on stale data, process mismatch, invalid data, map
change, failure to make progress, or its own deadline. No old coordinates should
survive a failed sample as if they were fresh. An output I/O failure must be
reported; atomic replacement prevents a reader seeing a partly written record.

`face yaw` needs a definition. Right-stick motion generally controls the camera,
whereas this chain reports the hunter's facing. Camera-relative `goto` also
needs a qualified camera heading, or a controlled calibration of stick-to-world
movement. bbhost has a separate camera position/focus chain; it has not been
checked here and should not be silently conflated with player yaw.

The first Playtest qualification should compare the stable spawn to the known
fixture XYZ, observe changes under a short walk and turn, distinguish camera
turn from character turn, then check menu, death, load/reload and map transition.
A normal save/quit gives an independent position cross-check. No position write
or warp is part of this tool. The likely implementation is one small runtime
module plus startup/input integration and a fixture for read failures, changing
chains, non-finite fields and atomic publication; it does not require renderer
or GPU-library changes.

## Implemented observer (qualification pending)

Set `BB_PLAYER_POS_FILE` to an absolute output path. Startup checks six short
1.09 code ranges after patches and before entry probes are installed. Each
successful pad read calls the observer **after unlocking the pad mutex**. It
samples at most every 100 ms; it does not create a timer or worker thread. File
creation/write/close/rename runs on that caller, so this diagnostic can add I/O
latency and is not recommended for performance captures.

Each observation uses two complete non-faulting self reads and requires equal
pointer chains, map and transform. This reduces inconsistent observations; it
does not establish guest object lifetime, simulation advancement, or input
controllability. Non-null unreadable pointers and non-finite XYZ/yaw publish
invalid output and terminate with exit 23. Null chain members, map `0xffffffff`,
and changing observations publish invalid output and a loud state-change line;
these are expected during loads. Invalid records omit player/XYZ/yaw/map fields.
File failure or code-guard mismatch also stops with exit 23. A failed write may
leave the previous file intact, so consumers must enforce freshness/process
identity regardless of the last record's validity.

Schema version 1 uses `pid`, a random hex `run_id`, `started_monotonic_ns`,
`sequence`, `sample_monotonic_ns`, `published_monotonic_ns`, `sample_age_ns`,
`pad_reads`, `valid`, `reason`, and `source="pad-read-observation"`. Valid records
add `player` (hex string), `xyz` (three floats), `player_yaw_rad`, and `map_raw`
(unsigned integer). Times use host CLOCK_MONOTONIC nanoseconds; sample age is
measured at formatting, before file I/O. `pad_reads` counts valid API calls, not
simulation frames. Consumers compute current age from the sample timestamp;
they must not treat a recent file mtime or repeated stationary XYZ as progress.

Playtest's agreed `goto` design does not need camera telemetry: it calibrates
left-stick forward against observed world displacement, keeps the right stick
untouched, then refines the stick-to-world mapping from later movements. It
must still detect camera auto-follow/collision effects through that feedback.
`face` is dropped. The first run remains observation-only before automated
steering is qualified. Camera-independent yaw sign calibration can be recorded
in that same run but is not required by `goto`.

The signed unit fixture exercises the production chain walk against fake
memory, null members, failed/overflowing reads, changing observations, invalid
map and non-finite values; JSON validity/field omission and sequence; loud
refusals; real Mach reads over a fake mapped image and protected/unmapped pages;
the rate cap and host/guest errno preservation. Its optional local image argument
checks the positive code guards without storing game bytes in the repository.

## Initial observation qualification

Playtest reported clean exits and no STOP for PREVIEW-28p (`34e2669`) runs
`20261012-120428-1080p-POSQUAL-1`, `20261012-120819-1080p-POSQUAL-2`,
`20261012-121308-1080p-POSQUAL-3` and `20261012-121717-1080p-POSQUAL-4`.
Their manifests pin bb-probe SHA256
`b356e62d867cd5dc276669e148265cb4e64d0b8744f368db63b323b8a724350a`
and libbbgpu SHA256
`e50c23fcf3c0e123117ec525f47645302fd70369ef1d20c313656e1e9268e348`.
Samples and screenshot-step associations are in each run's `positions.jsonl`
and `steps.jsonl` under Playtest's `out/macos_route`.

Reported visual comparisons support the known Central Yharnam spawn XYZ,
the Dream lantern position, XYZ changes during walking, and the second
coordinate as height. Left-stick direction changes turn player yaw; a
right-stick camera turn leaves that yaw unchanged. This supports planar
steering from `xyz[0]`/`xyz[2]` and keeps player yaw distinct from camera yaw.

Exit Game followed by Continue produced `absent-player-chain` records during
the title transition, then valid saved-position observations again. **Valid
can precede visible gameplay**: the first valid record appeared during loading.
The route must require its world-stage gate separately; validity does not mean
the player can be controlled. Live map transition and death were still pending
at this initial handoff, so automated navigation was not yet qualified.

An independent read of those four sample files found 421, 672, 985 and 1254
records (3332 total), strictly increasing sequences and one run ID per file.
The maximum publisher-reported age was 245000 ns (0.245 ms); the largest
interval between recorded sample timestamps was 674084000 ns (0.674 s).
These are observations, not worst-case freshness guarantees or evidence
that every pad read produced a file.

Consumer review found a blocker in the initial `PositionReader.fresh()`:
checking only `sample_age_ns` accepts an old file on first use because that
field records age at publication. Compute current age with
`time.clock_gettime_ns(time.CLOCK_MONOTONIC) - sample_monotonic_ns`, reject
invalid/future timestamps, and latch the run ID and start timestamp in
addition to PID. Test stale first use, a higher sequence with an old timestamp,
same-PID process replacement, and a stalled sequence. Playtest owns that
reader correction; this record does not claim it has landed.
