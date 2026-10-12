/* runtime_memory_inventory (capture-replay inventory) against the actual VMA table: view bytes,
 * the backing union and shared-backing views for disjoint views, duplicate and partial aliases,
 * flexible memory, reservations and unmaps (including a split). */
#include "../src/runtime_memory.c"
#include <assert.h>
#include <signal.h>
#include "test_alarm.h"
uintptr_t runtime_lookup(const RuntimeExport *table,size_t count,const char *name) {
    for (size_t i=0;i<count;++i) if (!strcmp(table[i].name,name)) return (uintptr_t)table[i].function;
    return 0;
}
static RuntimeMemoryInventory base;
static RuntimeMemoryInventory take(void) {
    RuntimeMemoryInventory m;
    assert(!runtime_memory_inventory(&m));
    return m;
}
/* Expected deltas from the baseline (views and bytes in pages). */
static void expect(const char *step,size_t direct,size_t flexible,size_t reserved,uint64_t direct_pages,
                   uint64_t flexible_pages,uint64_t unique_pages,size_t shared) {
    static uint64_t generation;
    RuntimeMemoryInventory m=take();
    printf("  %-34s direct %zu (%llu pages) flexible %zu reserved %zu unique %llu pages shared %zu\n",step,
           m.direct_vmas-base.direct_vmas,(unsigned long long)((m.direct_view_bytes-base.direct_view_bytes)/PAGE),
           m.flexible_vmas-base.flexible_vmas,m.reserved_vmas-base.reserved_vmas,
           (unsigned long long)((m.unique_backing_bytes-base.unique_backing_bytes)/PAGE),
           m.shared_backing_views-base.shared_backing_views);
    assert(m.direct_vmas-base.direct_vmas==direct);
    assert(m.flexible_vmas-base.flexible_vmas==flexible);
    assert(m.reserved_vmas-base.reserved_vmas==reserved);
    assert(m.direct_view_bytes-base.direct_view_bytes==direct_pages*PAGE);
    assert(m.flexible_view_bytes-base.flexible_view_bytes==flexible_pages*PAGE);
    assert(m.unique_backing_bytes-base.unique_backing_bytes==unique_pages*PAGE);
    assert(m.shared_backing_views-base.shared_backing_views==shared);
    assert(m.flexible_in_use_bytes-base.flexible_in_use_bytes==flexible_pages*PAGE);
    assert(m.generation!=generation); /* every step mutates the table */
    generation=m.generation;
}
int main(void) {
    test_alarm(30);
    runtime_memory_reserve();
    puts("Runtime memory inventory: actual VMA table"); fflush(stdout);
    assert(runtime_memory_inventory(NULL)==-1);
    base=take();
    assert(!base.shared_backing_views); /* nothing aliased before the fixture */
    int64_t physical;
    assert(!direct_allocate(0,POOL_SIZE,4*PAGE,PAGE,0,&physical));
    assert(take().live_direct_bytes-base.live_direct_bytes==4*PAGE);
    void *a=NULL,*b=NULL,*dup=NULL,*partial=NULL,*flex=NULL,*reserved=NULL;
    /* Disjoint: A = pages 0-1, B = pages 2-3. */
    assert(!direct_map(&a,2*PAGE,3,0,physical,PAGE));
    assert(!direct_map(&b,2*PAGE,3,0,physical+2*PAGE,PAGE));
    expect("disjoint views",2,0,0,4,0,4,0);
    /* Duplicate alias of A: both A and the duplicate share. */
    assert(!direct_map(&dup,2*PAGE,3,0,physical,PAGE));
    expect("duplicate alias",3,0,0,6,0,4,2);
    /* Partial alias over pages 1-2: overlaps A, the duplicate and B. */
    assert(!direct_map(&partial,2*PAGE,3,0,physical+PAGE,PAGE));
    expect("partial alias",4,0,0,8,0,4,4);
    /* Flexible memory has its own backing; a reservation has none. */
    assert(!map_flexible(&flex,3*PAGE,3,0));
    assert(!reserve_range(&reserved,5*PAGE,0,PAGE));
    expect("flexible and reservation",4,1,1,8,3,7,4);
    /* Unmapping the duplicate leaves A shared with the partial alias only. */
    assert(!direct_unmap(dup,2*PAGE));
    expect("duplicate unmapped",3,1,1,6,3,7,3);
    /* Unmapping the partial alias's first page (backing page 1) ends its overlap with A; its
     * remaining page (backing page 2) still shares with B. */
    assert(!direct_unmap(partial,PAGE));
    expect("partial alias page 1 unmapped",3,1,1,5,3,7,2);
    /* The partial alias's last page goes; releasing the middle of the flexible view splits it. */
    assert(!direct_unmap((char *)partial+PAGE,PAGE));
    assert(!release_flexible((char *)flex+PAGE,PAGE));
    expect("alias gone, flexible split",2,2,1,4,2,6,0);
    assert(!release_flexible(flex,PAGE));
    assert(!release_flexible((char *)flex+2*PAGE,PAGE));
    assert(!direct_unmap(a,2*PAGE));
    assert(!direct_unmap(b,2*PAGE));
    expect("all views unmapped",0,0,1,0,0,0,0);
    assert(!direct_release(physical,4*PAGE));
    puts("PASS: inventory counts views, backing union and shared views through aliases and unmaps");
}
