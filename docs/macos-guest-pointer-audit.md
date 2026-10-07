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
