#ifndef BB_RUNTIME_MEMORY_H
#define BB_RUNTIME_MEMORY_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#ifdef __APPLE__
/* Install all three callbacks together before enabling imported GPU buffers.
 * begin: outside the VMA lock, serialize mutations, stop new GPU uses and wait
 *        for submitted work/readers. Keep the renderer paused until end.
 * retire: under the VMA writer lock, destroy every import overlapping this guest
 *         range and invalidate its BDA entries. Do not wait or query VMAs here.
 * end: after the VMA lock is dropped AND queued map/unmap/invalidate callbacks
 *      have run synchronously, permit GPU work/import creation again.
 * Notifications between begin/end must operate on the drained caches directly.
 * These callbacks cover VM mutations, never per-draw lookups or tracking faults.
 * Imported ranges must be indexed by their guest views, including all VMAs in a
 * spanning import. A backing-pool-wide import independent of views is unsupported.
 * Callbacks must complete or fail loudly; do not return with GPU uses outstanding.
 * Install/remove at quiescent startup/shutdown; concurrent replacement is invalid.
 */
void runtime_memory_set_gpu_lifetime_hooks(void (*begin)(void),
    void (*retire)(uintptr_t address,uint64_t size),void (*end)(void));

/* Exact backing identity for import creation, not a per-draw query. Returns 1
 * for a committed VMA, 0 for a gap/reservation/untracked low allocation. offset
 * is the unified backing-pool byte offset at *start (direct and flexible occupy
 * disjoint intervals). Aliases have overlapping offset intervals. The caller
 * must participate in the lifetime protocol while using the snapshot: release
 * may replace the physical object at an identical offset after retiring imports.
 */
int runtime_memory_backing_region(uintptr_t address,uintptr_t *start,
    uintptr_t *end,uint64_t *offset);

typedef struct {
    uintptr_t start, end, address;
    uint64_t backing_offset; /* pool offset at start */
    int prot; /* guest protection, not temporary GPU tracking restrictions */
} RuntimeMemoryAlias;
typedef struct {
    uint64_t generation, backing_byte;
    uintptr_t start, end; /* queried VMA */
    int prot;
    size_t total, copied; /* other guest VMAs mapping backing_byte */
    int truncated;
} RuntimeMemoryAliasSnapshot;
/* Failure-path diagnostic: one VMA read lock, no allocation, GPU wait or logging.
 * Returns 1 for tracked committed memory (total==0 means no other guest alias),
 * 0 for a reservation/gap/untracked address, -1 for invalid output arguments.
 * On 0, snapshot is zero except generation. capacity==0 allows aliases==NULL.
 * Caller-owned writable outputs receive at most capacity rows, in VA order;
 * address in each row is the VA of the exact queried byte in that other view.
 * Host pool/import mirrors are not guest VMAs and are intentionally excluded.
 * This is a current-table snapshot, not mutation history or a lifetime pin.
 * generation can be odd when called within this thread's existing mutation.
 * Not signal-safe: do not call from a fault handler. Log only after it returns.
 */
int runtime_memory_alias_snapshot(uintptr_t address,RuntimeMemoryAlias *aliases,
    size_t capacity,RuntimeMemoryAliasSnapshot *snapshot);
typedef struct {
    uint64_t generation;
    size_t direct_vmas, flexible_vmas, reserved_vmas;
    uint64_t direct_view_bytes, flexible_view_bytes; /* sum of committed guest views */
    uint64_t unique_backing_bytes; /* union of the views' backing-pool intervals */
    size_t shared_backing_views;   /* committed views whose backing overlaps another view's */
    uint64_t live_direct_bytes, flexible_in_use_bytes; /* allocator counters */
} RuntimeMemoryInventory;
/* Capture-replay inventory (diagnostic). Grows its span buffer outside the lock until the
 * VMA table fits, then reads the table and the allocator counters under one read lock (one
 * generation); the backing union and shared views are computed after unlocking. Not for
 * fault handlers. Low/untracked host allocations outside the VMA table are not counted.
 * Returns 0, or -1 for a NULL output or failed allocation. */
int runtime_memory_inventory(RuntimeMemoryInventory *out);
typedef struct {
    uint64_t start, end;   /* guest VA */
    uint64_t phys;         /* backing offset (runtime_memory_backing) */
    int32_t flexible;      /* 0 direct, 1 flexible */
    int32_t prot, type, reserved;
} RuntimeMemoryView;
typedef struct { uint64_t start, size; int32_t type, reserved; } RuntimeMemoryBlock;
/* Capture-replay layout (diagnostic): the committed views in VA order and the direct
 * allocations in offset order, from one read of the VMA table (`generation` as in
 * RuntimeMemoryInventory). Writes up to each capacity and sets the total counts; a count above
 * its capacity means call again with more room. Returns 0, or -1 for a NULL count/generation or
 * a NULL array with a nonzero capacity. Not for fault handlers. */
int runtime_memory_layout(RuntimeMemoryView *views, size_t view_capacity, size_t *view_count,
                          RuntimeMemoryBlock *blocks, size_t block_capacity, size_t *block_count,
                          uint64_t *generation);
/* The host view of the backing pool (direct memory in [0, *direct_bytes), flexible memory after
 * it, *total_bytes in all), or NULL before the pool exists. Reads through it bypass guest and
 * GPU page protection: capture only. */
const void *runtime_memory_backing(uint64_t *direct_bytes, uint64_t *total_bytes);
/* The guest executable image as loaded (a host allocation outside the VMA table): 0 and its
 * range, or -1 before it is loaded. Defined by the launcher (probe.c). */
int runtime_guest_image(uintptr_t *start, uint64_t *size);
/* Writes through the backing view, bypassing guest/GPU page protection: 1, or 0 when part of
 * the range has no backing. */
int runtime_memory_write_backing(uintptr_t address, const void *data, uint64_t size);
/* Perf replay v0 (BB_REPLAY_RUN, after the GPU library is initialised): recreates the captured
 * direct allocations at their offsets, maps every captured view at its VA and fills it from
 * backing.bin. 0, or -1 with a reason in `why`. */
int runtime_memory_replay_map(const char *dir, char *why, size_t why_size);
#endif
#ifdef __cplusplus
}
#endif
#endif
