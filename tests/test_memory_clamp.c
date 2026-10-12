/* Actual backend range queries across guest and host mapping boundaries. */
#include "../src/runtime_memory.c"
#include <assert.h>
#include <signal.h>
#include "test_alarm.h"
uintptr_t runtime_lookup(const RuntimeExport *table,size_t count,const char *name) {
    for (size_t i=0;i<count;++i) if (!strcmp(table[i].name,name)) return (uintptr_t)table[i].function;
    return 0;
}
#define TEST_PAGES 20
static unsigned char mapped[TEST_PAGES];
static unsigned checks;
static void check_ranges(uintptr_t base,const char *stage) {
    const uint64_t sizes[]={0,1,864,4096,PAGE-1,PAGE,PAGE+1,8*PAGE,24*PAGE};
    const uint64_t offsets[]={0,1,255,PAGE-1};
    for (unsigned cache=0;cache<2;++cache) {
        runtime_disabled_optimizations=cache;
        for (unsigned page=0;page<=TEST_PAGES;++page) for (unsigned o=0;o<4;++o) {
            uintptr_t at=base+page*PAGE+offsets[o];
            uint64_t available=0;
            if (page<TEST_PAGES && mapped[page]) {
                unsigned last=page+1;
                while (last<TEST_PAGES && mapped[last]) ++last;
                available=(last-page)*PAGE-offsets[o];
            }
            uintptr_t region_start=0,region_end=0; int region_mapped=0;
            const int found=runtime_memory_region(at,&region_start,&region_end,&region_mapped);
            assert((found && region_mapped)==(available!=0));
            if (found) assert(region_start<=at && at<region_end);
            for (unsigned n=0;n<sizeof(sizes)/sizeof(sizes[0]);++n) {
                const uint64_t expected=available<sizes[n] ? available : sizes[n];
                const uint64_t got=runtime_memory_clamp(at,sizes[n]);
                if (got!=expected) fprintf(stderr,"%s cache=%u page=%u offset=%llu size=%llu got=%llu expected=%llu\n",
                    stage,cache,page,(unsigned long long)offsets[o],(unsigned long long)sizes[n],
                    (unsigned long long)got,(unsigned long long)expected);
                assert(got==expected);
                if (sizes[n]) assert(runtime_memory_is_mapped(at,sizes[n])==(expected==sizes[n]));
                ++checks;
            }
        }
    }
    runtime_disabled_optimizations=0;
    printf("PASS %s\n",stage);
}
int main(void) {
    test_alarm(30);
    puts("Memory clamp: actual host backend mapping test"); fflush(stdout);
    runtime_memory_reserve();
    int64_t phys;
    assert(!direct_allocate(0,POOL_SIZE,32*PAGE,PAGE,0,&phys));
    void *view=NULL;
    assert(!direct_map(&view,16*PAGE,3,0,phys,PAGE));
    const uintptr_t base=(uintptr_t)view;
    memset(mapped,1,16);
    check_ranges(base,"single direct VMA, interior vertex/constant ranges");
    assert(!kernel_mprotect((void *)(base+2*PAGE),PAGE,1));
    assert(!kernel_mprotect((void *)(base+3*PAGE),PAGE,0));
    check_ranges(base,"guest protection splits, including mapped PROT_NONE");
    assert(!kernel_mprotect((void *)(base+2*PAGE),2*PAGE,3));
    runtime_memory_gpu_protect(base+4096,4096,1,0);
    check_ranges(base,"4-KiB GPU protection split, unchanged guest VMA extent");
    void *at=(void *)(base+5*PAGE);
    assert(!direct_map(&at,PAGE,3,MAP_FIXED_FLAG,phys+24*PAGE,PAGE));
    check_ranges(base,"adjacent direct VMAs with noncontiguous physical offsets");
    at=(void *)(base+6*PAGE);
    assert(!map_flexible(&at,PAGE,3,MAP_FIXED_FLAG));
    check_ranges(base,"direct/flexible/direct boundaries");
    at=(void *)(base+16*PAGE);
    assert(!direct_map(&at,4*PAGE,3,MAP_FIXED_FLAG,phys,PAGE));
    memset(mapped+16,1,4);
    check_ranges(base,"adjacent physical alias extends contiguous virtual range");
    assert(!direct_unmap((void *)(base+8*PAGE),PAGE)); mapped[8]=0;
    check_ranges(base,"unmap invalidates warm cache and stops at actual hole");
    at=(void *)(base+8*PAGE);
    assert(!reserve_range(&at,PAGE,MAP_FIXED_FLAG,PAGE));
    check_ranges(base,"reservation is not mapped backing");
    assert(!direct_map(&at,PAGE,3,MAP_FIXED_FLAG,phys+8*PAGE,PAGE)); mapped[8]=1;
    check_ranges(base,"reservation replacement restores full contiguous range");
    assert(!direct_release(phys+24*PAGE,PAGE)); mapped[5]=0;
    check_ranges(base,"physical release removes only corresponding mapped view");
    assert(!direct_unmap((void *)base,TEST_PAGES*PAGE));
    printf("PASS memory clamp: %u cached/uncached range checks\n",checks);
}
