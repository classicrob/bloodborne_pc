#ifndef BB_RUNTIME_MEMORY_H
#define BB_RUNTIME_MEMORY_H
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
#endif
#ifdef __cplusplus
}
#endif
#endif
