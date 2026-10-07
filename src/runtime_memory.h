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
#endif
#ifdef __cplusplus
}
#endif
#endif
