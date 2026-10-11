# GPU capture and offline replay: feasibility and proposed scope

Design study, 2026-10-10. No capture/replay implementation is included. Source audit
against macos through `4096d7d`; later fault diagnostics do not change this design.

## Recommendation

Build a bounded **Liverpool input replay**, using the real command processor,
shader compiler, buffer/texture caches and renderer, with the real Mach memory
backend and no guest CPU execution. Start with one completed graphics frame,
including its compute dispatches, from a checkpoint with all earlier work retired.
Compare a game render target before host upscaling/presentation. This is a useful
route to repeatable GS, depth-bounds, shader and image-coherence reproducers.

A command-buffer archive alone cannot do this. It lacks live resource contents,
initial renderer state, CPU-written predicates, and GPU-resident images whose
contents are newer than guest RAM. The principal implementation risk is obtaining
consistent versions of those inputs while the game is running. Make that the first
feasibility gate, before building a polished replay runner.

This will reproduce a parser defect in a recorded stream. It does **not** by itself
reproduce the original guest/command-processor race: submit snapshots deliberately
remove subsequent guest mutations. A race reproducer additionally needs observed
memory versions and externally produced writes with their ordering. Nor is replay
without guest execution a reproducer of the movie's guest resource-lifetime fault.
It can isolate whether GPU outputs feeding that fault are wrong.

## What has to be recorded

Use explicit, versioned records, not raw C++ object dumps. Native pointers, Vulkan
handles, coroutines, mutexes and host buffer-device addresses are not portable state.

| Input or state | Capture point and required contents |
| --- | --- |
| Graphics submissions | `Liverpool::SubmitGfx` and `CmdSnapshot`: submitted DCB/CCB bases, sizes **with units**, copied words, recursively reached graphics/constant IBs, constant/graphics classification, submit order and chain/jump semantics. Keep duplicate address generations distinct. |
| Async compute | `SubmitAsc`, queue mapping and compute IB reads: queue ID, ring base/size/read-pointer address, submitted segments, split-packet state and recursive IB bytes. Graphics snapshots currently do **not** cover this path. A graphics PM4 compute dispatch is different from an ASC queue submission. |
| Register/context checkpoint | Full `Regs::reg_array` (0xD000 dwords, 208 KiB), color/depth extent hints, per-queue compute registers, indirect-argument base, counter state and CE state, including its 48 KiB constants heap. Explicitly restore cache dirty state so the first replay draw builds the correct pipeline. |
| Queue state | A quiescent cut: no partially executed parser coroutine, pending host command, deferred signal, draw preparation, copy, fence or GPU work from before the cut. Serialize semantic queue mappings/counters, not those live tasks. If this cut cannot be reached, fail capture. |
| Shader inputs | Original GCN program bytes for every stage, including fetch, ES/GS/copy programs, headers/lengths, MODE, user-data registers, runtime specialization inputs and descriptor/sharp tables. Capture on lookup even on warm shader-cache hits. Recompile through the production compiler; dumped SPIR-V is useful supplementary evidence. |
| Buffer inputs | Full enclosing extents of vertex/index, uniform/storage, indirect/count/argument, flat-buffer and BDA-accessible resources. Capture initial bytes even for a writable binding: blending, atomics and read-modify-write need old contents. Include CPU-side descriptor pointer walks before `ObtainBuffer`. |
| Texture state | Image identity, guest address/size, stencil plane, mip/layer layout, pitch, tiling/swizzle, format/aspects, sample count and metadata such as CMASK/FMASK/HTILE. Preserve the **authoritative initial contents**, which may be in a live Vulkan image instead of guest backing. |
| Memory topology | Allocated extents, all guest views, backing identity/offset, permissions, aliases and allocation generation; ordered map/unmap/release/zero events within the window. Include low shader/image heaps and other mappings outside the direct/flexible pool. Restore exact guest VAs. |
| Parser memory inputs | `WAIT_REG_MEM`, `COND_EXEC`, indirect counts/addresses, CE uploads, query destinations, rewind/refetch memory, labels and other packet-read memory. Associate each observed version with its queue and packet position. |
| External events | CPU-produced memory changes, VideoOut label/vblank inputs, recorded clock inputs where observable, and registration/mapping changes. Identify which events are CPU-produced versus GPU-produced. |
| Output oracle | Draw/dispatch counts, pipeline/shader identities, selected buffer/label values and images at named consumer/retirement points. Record format, aspect, extent, row layout and hashes, not just PNGs. |
| Provenance | Capture schema, source commit, build hashes, OS/MoltenVK/device properties, enabled profile/capabilities, game/content hash, resolution, relevant settings/toggles and frame/submit IDs. Keep proprietary payloads in local `out/`, outside git. |

Recreate host-internal BDA tables, descriptor allocations, generated GS buffers and
other derived resources through the production renderer. Never restore captured
host addresses into a new process. Guest VAs embedded in shaders or PM4 must retain
their meaning. Backing aliases must share bytes, not become independent copied pages.
A reused guest VA is a new generation, not an update to an eternal resource.

`runtime_memory_backing_region` helps identify direct/flexible views and their pool
offsets. It is not an enumeration/checkpoint API, and a failed query is not evidence
that memory is irrelevant: low allocations and other mappings need explicit coverage.
The lifetime Begin/Retire/End hooks establish GPU retirement before backing changes;
they do not suspend all guest CPU or HLE writers.

## Finding the ranges

### Resource and packet hooks: necessary, but not sufficient alone

Hook PM4 memory reads, shader/fetch-code lookup and sharp/flat-buffer construction,
`BufferCache::ObtainBuffer`, and texture-cache image/view lookup. `BindBuffers`
already knows a sharp's base and declared size, and image descriptions contain the
full tiled allocation/subresource extent. Record those enclosing ranges, then dedupe
by backing identity and generation. Add explicit index and indirect-argument ranges;
there is more input than the current frame report's buffer sample.

These hooks find **declared resources**, not a proof of every byte a shader will
read. Dynamic indexing, BDA and pointer-derived descriptors can widen the closure.
Retain a conservative baseline containing all allocated backing for v1, and use the
hooks to audit coverage and explain size. Later trim only when the address-resolution
path proves the omitted range cannot be accessed, or with shader instrumentation.
A range hook encountered after GPU execution cannot recover its initial bytes.
Take the baseline first; keep versions until dependencies have been resolved.

### Page-protection capture: useful CPU evidence, not GPU touch tracing

Rosetta reports 4 KiB host pages; the runtime's guest allocation page is 16 KiB.
CPU read protection can identify first CPU reads, and write protection can identify
first CPU writes per epoch. Integrate any capture protection with the existing GPU
write tracker and every alias; do not replace its protection decisions. Fault handlers
would only append to preallocated records, with no allocation or file I/O.

This cannot discover all shader reads. Metal accesses the imported backing through
its own resource mapping/RW mirror; our host-import tests already show GPU writes
succeed while the guest view is read-only. No guest CPU fault is delivered for that
GPU access. Protecting guest pages therefore cannot make a GPU capture complete.

First-write tracking also does not record every subsequent store after a page is
unprotected. Nor does it distinguish GPU and CPU byte changes on a shared page.
Using those page deltas to overwrite replay memory could hide a wrong GPU result.
Full read protection adds faults and TLB work to unrelated CPU accesses and will
substantially perturb timing. Do not make it the initial primary capture mechanism.
Mach copy-on-write/remapping is not assumed to provide a snapshot isolated from
ongoing GPU writes; that property has not been qualified.

### Full allocated backing: simplest coverage baseline

Capture allocated backing, not the 560 GiB virtual reservation from 448 to 1008 GiB.
Direct backing defaults to 5056 MiB and `BB_DMEM_MB` can raise it to 16384 MiB; the
flexible backing section spans another 1 GiB. Unallocated/known-zero extents can be
represented without payload. Store aliases once with multiple view records.

This baseline closes many missed-resource problems at the cost of seconds of copy
and storage work. It still does **not** capture newer GPU images, topology outside
the pool, or CPU writes made after the checkpoint. It is a debugging capture cost,
not per-frame production work. Record actual allocated/unique bytes before starting
and refuse a configured disk/memory quota instead of silently truncating.

## Consistency protocol and the v1 feasibility gate

A replay file must describe a coherent execution, rather than combine register state
from one submit with texture contents from another.

1. Arm at an explicit submit/frame boundary. Stop accepting later submissions, retire
   earlier Liverpool/draw/copy/GPU work, and reach a CPU-writer safe point. A VideoOut
   flip number alone does not prove that all graphics and compute work has retired.
2. Park relevant guest and HLE producers **cooperatively outside host locks** while
   copying the memory/topology checkpoint and authoritative image state. GPU-idle
   alone is insufficient: AvPlayer and guest threads can still write memory. There
   is no general cooperative checkpoint service today. Add bounded rendezvous at
   designated runtime/submit boundaries; report threads that cannot rendezvous.
   Do not suspend arbitrary Mach threads while they might own a lock the capture
   needs, or assume every thread eventually calls an HLE function.
3. Resume producers for the bounded capture window. Record submissions and externally
   produced memory versions at explicit consumption boundaries. For v1, deliberately
   serialize capture epochs: complete earlier GPU work before admitting a new input
   version. Label the capture as serialized; it is a renderer reproducer, not evidence
   of the original overlap/race timing.
4. Qualify v1 first on a frame whose resource inputs are immutable during each GPU
   epoch. CPU-authored updates between epochs must be isolated from GPU output bytes.
   If CPU and GPU both write a range without a provable boundary, or an external
   producer cannot be parked, reject it. A whole-page delta is not permission to
   replace GPU output. Instrumentation/scheduling needed to admit these cases belongs
   in v2, not a silent guessed order in v1.
5. Capture post-work reference outputs with the same ordering as the live consumer,
   close the file only after every payload/hash is complete, and validate its input
   closure. Keep incomplete captures visibly incomplete and non-replayable.

The first spike should test whether a settled character-preview frame satisfies this
protocol. If not, reduce to an isolated draw/dispatch interval with a valid checkpoint,
or explicitly expand the producer instrumentation. Do not promise arbitrary live
one-frame capture on the strength of the existing command snapshots.

### Inventory run (implemented, diagnostic)

`BB_REPLAY_CAPTURE=inventory:<frames>` (Apple, off by default; `BB_REPLAY_HOLD_MS`
bound, 1–60000, default 3000; malformed values disable it loudly) tests step 1 only.
At the `<frames>`th *retired* frame end (sceGnmSubmitDone after which GpuIdle was
raised with nothing queued; a frame end that finds submissions already queued is not
counted, so the held frame can be later than `<frames>` + 1) the GPU command thread
stops parsing until the guest ends the next frame, then prints:

- G1: whether the cut was reached, renderer quiescence, and blocking host commands that
  ran during the hold (buffer synchronize, import pause; they must run or the guest
  never ends the frame, and each one rejects the cut). A CPU (non-EOP) flip ends the
  hold and rejects the cut: it would present a framebuffer whose draws are held. The
  flip, and commands queued behind it, then wait until the held work has parsed, so
  the game does not present or publish the frame early. A held submission waiting on
  memory that only a command behind the flip would write cannot finish (as with any
  command queued behind a flip).
- G2: a raw scan of the held frame's copied command regions (potential packets,
  including untaken COND_EXEC/predicated ranges; uncopied, invalid or truncated regions
  make it incomplete and reject), graphics and asynchronous-compute tasks queued at the
  cut (sampled from the queues, plus asynchronous compute counted from sampling to
  the report), the runtime memory inventory and every GPU-modified
  image with its writeback reject reason.

The best verdict is "no inventoried blocker found; capture NOT yet qualified": the run
does not rendezvous guest CPU or HLE writers (step 2), omits low/untracked memory, does
not exercise image writeback, and cannot attribute asynchronous compute submitted
between the report and the released frame's retirement (reported when the window closes).

### Authoritative images are a separate checkpoint problem

Buffer imports share guest backing, but texture-cache images can hold newer contents.
Copying stale guest texture bytes loses the very coherence bug we want to reproduce.
Export the initial live image contents and the cache's authority/alias metadata, then
seed equivalent images in replay. Preserve mip/layer/aspect contents and mark layouts
and cache state consistently. A pending clear or GPU-modified texture is meaningful
state, not a reason to re-upload stale RAM.

For v1, implement and qualify only an explicit format/sample set. Single-sample color
and depth/stencil must preserve the actual aspects; a depth-only readback cannot
stand in for depth+stencil. Compressed, packed and multisampled images need additional
work; resolving MSAA changes semantics and cannot be a generic replacement. Reject
unsupported initial states, or extend the captured prefix so the real commands build
that image from supported inputs. Such a prefix also helps temporal dependencies.

Host FSR/TAA history is outside a pure Liverpool checkpoint. Initially compare the
native game render target before host upscaling, clearly label that output boundary,
and retain the game's real rendering commands. Reproducing the presented frame needs
host temporal-state capture or a warm-up prefix as well.

## Dependencies without a guest CPU

Replay is an input driver, not a replacement PS4 CPU emulator. It submits the recorded
streams and injects only recorded **external inputs**, with explicit queue/packet or
completion dependencies.

| Dependency | Replay behavior |
| --- | --- |
| GPU writes a label/EOP value and a later GPU packet waits on it | Execute the real write and wait. Validate the value and ordering; never force the predicate true. |
| Guest CPU reads a completed label and submits more work | No CPU read is needed. Release the next recorded submit after the corresponding real completion. Log the outbound interrupt/event as an observable result. |
| Guest CPU changes a predicate/resource | Apply the recorded CPU input version at its established dependency boundary. Do not patch a GPU-produced value from the reference just to make progress. |
| `COND_EXEC` | Read replay memory through the real parser. Log and compare the branch decision; divergence is a failure, not an instruction to take the recorded branch. |
| VideoOut labels/vblank/flip callbacks | Provide the recorded port/registered buffers and a small replay event adapter with deterministic scheduled external events. Retain real completion ordering. `SetVoPort`/wait code currently expects a port; a null port or unconditional-success stub is insufficient. |
| Timestamp/query consumed by later work | Distinguish virtual external time from GPU query results. Define the virtual-time input contract and validation rule. Reject unsupported observable query behavior in v1; don't overwrite computed results indiscriminately. |
| `REWIND` or CPU-patched live command memory | v1 rejects. Current snapshots intentionally switch REWIND parsing back to live memory. v2 needs recorded live versions and refetch ordering. |
| ASC cross-queue waits | v1 rejects a required uncaptured ASC queue. v2 records those queues and their dependencies, rather than linearizing them silently. |

Bound waits by a diagnostic timeout reporting queue, packet, address, actual value,
comparison and missing producer. Record all output writes before presenting success.
Replaying the same inputs cannot guarantee bitwise determinism for a shader with
unordered competing atomics or undefined/uninitialized reads. Identify those cases
and their comparison contract explicitly; do not call their nondeterminism a capture
success or hide it with broad image tolerances.

## Existing pieces to reuse

| Piece | Reuse and limit |
| --- | --- |
| `CmdSnapshot` | The graphics words and recursive-IB map are a strong starting point. Add a serialization/export interface; preserve unreadable-but-unexecuted regions and fail if replay executes a missing region. Current bounds are 4096 regions and 64 MiB per submission. Not an ASC or full memory capture. |
| `BB_CAPTURE_TRIGGER` / `vk_frame_capture` | Reuse arming, frame/pass IDs and the readable graph. Its passes aggregate draws, shaders and a bounded set of sampled images/buffer samples; it is not an exhaustive event or resource archive. Align a new explicit submit cut with its reported frame. |
| `BB_DUMP_SAMPLED` | Reuse ordered image-to-staging readback and completion-delayed file writing, plus consumer-side reference images. Currently restricted to selected addresses, mip/layer zero and limited single-sample formats; it is not a complete image checkpoint. |
| `BB_FRAME_DUMP_TRIGGER` / shader dumps | Output oracle and human inspection. Capture original shader inputs on cache hits too, rather than relying solely on compile-time dumps. |
| Mach memory/lifetime backend | Exact low VAs, aliases, release-zero semantics, import retirement and safe reads. Needs state enumeration/restore and a replay mapping API. Keep required imports working through the real backend. |
| GPU test bootstrap | Reuse Vulkan setup and signed test conventions. `tests/test_gpu_runtime_stubs.cpp` deliberately aborts on guest-memory and host-worker calls; it cannot run Liverpool replay unchanged. |
| BB_PERF schema 3 | Reuse Stage A/Stage B/wait counters and qualified GPU timing. Capture overhead and replay restore time require separate accounting. |

`bbgpu_init` also creates a window/presenter and registers HLE libraries. Factor a
small renderer bootstrap shared with the replay executable, retaining production
initialization and profile decisions. The runner needs memory, thread and possibly
main-thread UI services, but must not enter eboot/native guest modules, start AvPlayer,
or run guest callbacks. An initial windowed runner is acceptable; headless operation
is a later convenience, not a reason to bypass required VideoOut semantics.

## File layout and size budget

Proposed directory: manifest plus typed state/events and content-addressed binary
chunks. All addresses are guest addresses or stable object IDs. Encode integers,
endianness and size limits explicitly. Hash payloads; validate bounds/overlaps and
references before mapping. Use atomic completion marking. Never trust a captured
host pointer or serialized allocation length without validation.

Deduplicate immutable shader/texture data and physical aliases. Version mutable
chunks by epoch; 16 KiB backing chunks match guest pages, while a 4 KiB dirty mask
can reduce later diffs. Store zero chunks symbolically. Compression ratio is unknown
and must not be part of the capacity guarantee.

| Component | Size evidence or planning bound |
| --- | --- |
| Graphics command snapshots | Recent efce883 loading logs report 137–155 KiB per reported frame, 4–5 us mean copy per submit, with 13 ms outliers. This is command-copy accounting, not a full replay size measurement. |
| Register/CE checkpoint | 208 KiB register bank + 48 KiB CE heap, plus queue/context metadata. |
| One 784x784 image | 2.34 MiB at 4 bytes/pixel; 4.69 MiB at 8 bytes/pixel, before mip/plane metadata. Several such preview targets are live. |
| One full-size RGBA8 target | 7.91 MiB at 1080p; 31.64 MiB at 4K. Histories, depth/stencil and HDR increase this. |
| Conservative direct/flexible baseline | Up to the configured direct pool (default 5056 MiB, maximum 16384 MiB) plus the 1 GiB flexible backing span; allocated-byte enumeration can reduce it. Add low/other mappings and authoritative images separately. |
| Trimmed single-frame payload | Budget provisionally 0.1–1 GiB **as an estimate**, not a measured working set. Large texture sets or a loading frame can exceed this. Full-baseline captures can be several GiB or more. |
| Consecutive frames | Initial checkpoint + unique immutable data + ordered CPU deltas + commands + chosen oracles. Not N independent full dumps. GPU-generated changes should be regenerated, not stored as replacement inputs. |

Upload counters (for example 120–232 MB windows in the loading logs) are neither
unique input bytes nor a size estimate for a frame capture: uploads may repeat, and
resident untouched textures are absent from those counts. First report an inventory
of unique backing/image bytes and required epochs. At even 100 MiB per frame, 60 FPS
continuous capture means roughly 5.9 GiB/s before compression. This is an opt-in,
bounded-N tool. Use bounded staging and hard quotas, never discard a required input
to stay within them. Report any planned capture above the team's 2 GiB threshold.

## Staged delivery and effort

Estimates are engineering days for one experienced owner with memory/renderer review,
not elapsed-time promises. No renderer implementation is authorized by this study.

| Stage | Deliverable and acceptance | Estimate |
| --- | --- | --- |
| Feasibility gate | Inventory one preview frame; establish safe producer rendezvous, image-authority export and input-version closure. Demonstrate that capture-induced serialization is explicit and unsupported overlap is detected. Decide whether the chosen frame is admissible. | 2–4 days |
| v1: one graphics frame | Versioned archive, conservative initial backing + qualified image checkpoint, graphics/CE/IB serialization, real Liverpool/renderer runner, bounded external-input adapter and output oracle. Include in-graphics compute; reject required ASC/live-refetch/unsupported checkpoint state. Same-build replay produces matching draws/dispatches, labels and selected target contents with no guest CPU. | 7–12 additional days |
| v2: multi-frame | ASC/ring capture, ordered CPU input versions and map/release/alias generations, cross-queue/event dependencies, live-refetch where needed, broader image formats and optional temporal warm-up. Add a separate observed-mutation trace for race reproducers. | 10–20 additional days; arbitrary CPU-write race capture may exceed this |
| v3: performance A/B | Deterministic reset, prewarmed and cold-cache modes, repeated alternating runs, stage/wait/GPU timing, batch comparison of identical captures across revisions; no capture work in timed intervals. | 3–5 additional days |

Useful checkpoints need not wait for every stage. A single offending GS draw plus its
full initial state may be the first valuable artifact. Do not claim a standalone
frame reproducer merely because a synthetic draw fixture runs.

Required tests: reject a missing input; preserve physical aliases and release/reuse;
restore a GPU-modified image whose RAM is stale; exercise a CPU predicate and a real
GPU-produced label; replay constant-engine and chained-IB state; verify warm-cache
shader capture; fail unsupported ASC/REWIND and unsupported image aspects explicitly.
Run the same capture twice from clean replay state and compare intermediate outputs,
then change one resource byte/packet deliberately to prove the oracle notices.
Use exact checks for integer/label state. Define floating-image comparisons narrowly
for the tested device/path, and retain raw images for divergence diagnosis.

For performance, restore all mutated buffers/images/context state outside the timed
interval before each iteration. Distinguish cold shader compilation, warm recording,
GPU execution and restore overhead. Preserve production scheduling inside the replay
window; per-submit capture drains must not become the benchmark schedule. Standalone
renderer throughput excludes guest CPU work, streaming and game synchronization: it
is an A/B tool, not proof of end-to-end 4K60.

## Source map

- `gpu/shadps4/video_core/amdgpu/cmd_snapshot.{h,cpp}`: graphics snapshots and limits.
- `gpu/shadps4/video_core/amdgpu/liverpool.{h,cpp}`, `regs.h`: parser, queue/context,
  CE, waits, conditional execution and live memory accesses.
- `gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp`: bindings, shader/target
  use and sampled-image readback.
- `gpu/shadps4/video_core/buffer_cache/` and `texture_cache/`: imported backing,
  resource range discovery and image authority.
- `gpu/shadps4/video_core/renderer_vulkan/vk_frame_capture.{h,cpp}`: current graph
  capture and trigger boundaries.
- `src/runtime_memory.{c,h}`, `gpu/shim/bbgpu.cpp`,
  `tests/test_gpu_runtime_stubs.cpp`: mapping/lifetime and standalone bootstrap.
- `docs/macos/command-snapshots.md`, `parse-lag.md`, `perf.md`: existing command,
  scheduling and measurement evidence.
