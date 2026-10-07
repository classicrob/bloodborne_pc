# macOS guest-visible pointer audit

The guest packs pointers into 40 bits. Host heap and host TLS addresses on macOS cannot be assumed to fit. Audit covers runtime source allocation sites and exported pointer-returning contracts; opaque native internals reachable only by the host are not guest pointer contracts.

| Source / allocation | Guest exposure | Required action / owner |
| --- | --- | --- |
| runtime_thread.c new_thread: GuestThread | ScePthread/self handle; TCB thread field | Low calloc; Astra-1 notified |
| runtime_thread.c attr_init: ThreadAttr | Thread attribute handle | Low calloc/free; Astra-1 notified |
| runtime_thread.c attach: aligned TLS block / TCB | Rewritten GS load; static guest TLS | Low mapping, preserve TLS alignment; Astra-1 notified |
| runtime_thread.c attach: static dtv[3] | TCB DTV pointer | Keep DTV in low thread storage; Astra-1 notified |
| runtime_thread.c guest_errno | Address returned by __error/runtime_errno | Move from native thread-local scalar into low thread storage; Astra-1 notified |
| runtime_thread.c create: guest stack | Stack pointers passed by guest | Existing runtime_low_map; disallow high fallback on macOS; Astra-1 notified |
| runtime.c guest_tls_get_addr: per-module calloc | Dynamic TLS pointer returned to guest | Low calloc; Astra-2 |
| runtime_mutex.c attr_init: GuestAttr | Mutex attribute handle | Low malloc/free, including error cleanup; Sol-2 notified |
| runtime_mutex.c create: GuestMutex | Mutex handle, including lazy static initialization | Low malloc/free; Sol-2 notified |
| runtime_mutex.c cond_init: GuestCond | Condition handle, including lazy initialization | Low malloc/free; Sol-2 notified |
| runtime_rwlock.c create_locked: Rwlock | Rwlock handle | Low calloc/free; Sol-2 notified |
| runtime_rwlock.c acquire: Holder | Host-only registry/ownership tracking | Keep host calloc/free |
| runtime_sema.c sem_create: Sema | Guest receives monotonic uint32 ID, not pointer | Keep host calloc/free; Sol-2 notified |
| runtime_file.c File table / Listing / names / offsets / types | Integer descriptors; directory entries copied into caller buffer | Keep host allocations; Sol-2 notified |
| runtime_kernel.c keys/signal mask | Integer keys; stored values are guest-provided pointers; masks copied | No new low allocation; Sol-2 notified |
| runtime_audio.c ports / SDL objects | Integer handles; guest provides sample buffers | Keep host allocations; Sol-2 notified |
| runtime_ajm.c contexts, codec allocations, joined input | Integer context/instance IDs; writes to supplied guest buffers | Keep host allocations |
| runtime_pad.c replay / SDL objects | Integer pad ID; data copied into caller buffer | Keep host allocations |
| runtime_savedata.c directory entries | Sorted temporary host list; results copied into guest buffer | Keep host allocations |
| runtime_services.c net_errno | Address of static exported scalar | Main executable static; validate low placement (same as code/static imports) |
| runtime_services.c HTTP epoll | Integer ID cast to void*, never dereferenced | Already low |
| runtime.c stack_canary / need_libc_internal | Static data import addresses | Main executable static; validate low placement |
| runtime.c application_heap_api / process_param | Stored pointers originating in guest image/caller | Already guest-owned |
| runtime.c exit handlers and guard owner | Host registry / native TLS, values only | Keep host allocations |
| runtime_memory.c vmas / HostRegion / ProtRun / backing view | Host bookkeeping / private write alias | Keep host allocations / unrestricted backing alias |
| runtime_memory.c direct / flexible / reservations | Guest addresses and GPU descriptors | Reserve PS4 VA arena; shared fixed views below 1 TiB |
| probe.c allocate: image, trampolines, bindings visible to guest | Executable/data guest image and import trampolines | Existing runtime_low_map; disallow high fallback on macOS; Sol-2 notified |
| probe.c main stack | Guest entry stack | Existing runtime_low_map; no high fallback |
| probe.c binding arrays / segments / relocs / names | Loader bookkeeping only | Keep host calloc |
| runtime_resolve and runtime_*_resolve / GPU exports | Function addresses in guest imports | Main executable/dylib code: validate placement before handing imports to guest |

Planned API in runtime.h: runtime_guest_malloc(size), runtime_guest_calloc(count,size), runtime_guest_free(ptr). macOS uses a reusable low-address allocator backed by runtime_low_map; other platforms use their unchanged libc allocation routines. TLS blocks can use runtime_low_map directly for page alignment and process-lifetime allocation. No blanket malloc macro substitution.

## GPU-library exports

The compiled library export groups are GnmDriver, VideoOut, AvPlayer, and Kernel Equeue.

| Source / allocation | Guest exposure | Action |
| --- | --- | --- |
| avplayer/avplayer.cpp sceAvPlayerInit/InitEx: new AvPlayer | Pointer handle returned directly | Low allocation required; Sol-3 notified |
| videoout/video_out.cpp: make_unique<VideoOutDriver> | main_port.buffer_labels.data() returned to guest and written into GNM wait packets | Low allocation required for driver or label storage; Sol-3 notified |
| avplayer GuestBuffer / texture and audio frame buffers | Guest memory-replacement callbacks allocate output storage | Guest-owned, already low |
| avplayer state/source/streamer, FFmpeg AVIO buffer | Host-only decoder internals | Keep host allocations |
| kernel/equeue.cpp: new EqueueInternal, event timers | Integer equeue ID from NextEqueueHandle | Keep host allocations |
| gnmdriver Liverpool/Presenter and renderer objects | Host-only; command buffers/read pointers are guest-provided | Keep host allocations |

## Verified backend design

Rosetta on the test host has an immutable guard at 63–448 GiB (Astra-1 spike).
Host-owned guest objects use the reserved 32–63 GiB arena; guest direct/flexible
mappings use 448–1008 GiB. Mach fixed allocations first claim these arenas without
overwriting existing mappings. Anonymous replacements retain ownership of holes.

POSIX shm supports coherent data aliases but executable aliases fault under
Rosetta. The backend instead uses `mach_vm_remap(copy=FALSE)` from anonymous
backing storage, which supports shared data and executable views. Physical
release first removes all guest aliases, then replaces the released backing
subrange with fresh anonymous pages. This guarantees zero reuse and releases
physical storage without memset-committing unused pool pages.

`runtime_guest_malloc/calloc/free` uses reusable, 64-byte-aligned blocks from
low mappings. Freed neighboring blocks coalesce. Dynamic module TLS uses this
allocator; direct low mappings remain available for page-aligned TLS and stacks.

Run `sh tests/test_memory_macos.sh` for SDK-only x86_64/Rosetta regression checks.
They cover concurrent low allocation/reuse, aliases, protected backing writes,
partial release and zero reuse, remapping across split backing objects, arena
bounds, reservation holes, flexible offsets, and executable views. Modifying
already-translated instructions through the private backing alias requires an
RW-to-RX guest-view protection cycle to invalidate Rosetta's translation; the
ordinary instruction-cache builtin and reapplying RX alone did not suffice.
The backend now performs an execute-permission cycle on every executable alias
of physical pages modified through its backing-write API, retaining the actual
GPU-restricted protection. Tests cover writes addressed through both executable
and non-executable aliases followed by immediate execution.

## GPU follow-through

AvPlayer and VideoOutDriver now use class-specific low allocation on Apple.
VideoOut's inline label array therefore remains below 1 TiB as well. Native
internal vectors, decoder state, timers, and integer-ID objects stay on the host
heap. No global C++ allocation override is used. Allocation failure aborts with
a diagnostic naming low guest-visible GPU allocation.

The callback audit also found host pointers passed to guest code without being
returned as handles:

| Boundary | Low-pointer implementation |
| --- | --- |
| AvPlayer::OpenFile filename | Copy the complete NUL-terminated path into low temporary storage |
| AvPlayer::ReadOffsetFile FFmpeg AVIO buffer | Copy bytes into low temporary storage, invoke guest read callback, then copy all bytes back, preserving untouched bytes and the exact callback result |
| AvPlayerState warning payload | Copy the s32 payload into low temporary storage and copy changes back; other implemented event payloads are null |
| AvPlayer helper thread guest callback stack | Native std::jthread stack was high; assigned to Astra-1 by orchestrator for a low-stack implementation |

`sh tests/test_guest_alloc_macos.sh` tests C++ new/make_unique/delete dispatch,
64-byte alignment, member addresses, destruction, filename copies, partial read
buffer mutation with untouched bytes retained, and event payload copy-back using
the actual low allocator. The changed AvPlayer sources and VideoOutDriver header
also pass x86_64 syntax checks against the shared dependency headers. Full GPU
link/runtime verification remains with Sol-1's single build during the disk freeze.
