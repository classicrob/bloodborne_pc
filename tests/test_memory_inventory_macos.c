/* runtime_memory_inventory (capture-replay inventory) against the actual VMA table: view bytes,
 * the backing union and shared-backing views for disjoint views, duplicate and partial aliases,
 * flexible memory, reservations and unmaps (including a split). */
#include "../src/runtime_memory.c"
#include <assert.h>
#include <signal.h>
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
    alarm(30);
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
    /* Layout: every committed view with its backing offset, the direct block, and bytes written
     * through a guest view visible through the backing pool at that offset. */
    {
        RuntimeMemoryView views[64]; RuntimeMemoryBlock blks[64];
        size_t nv=0, nb=0; uint64_t generation=0, direct=0, total=0;
        assert(runtime_memory_layout(NULL,1,&nv,blks,64,&nb,&generation)==-1);
        assert(!runtime_memory_layout(views,64,&nv,blks,64,&nb,&generation));
        assert(generation==take().generation);
        size_t small=0, small_blocks=0; uint64_t g2=0;
        assert(!runtime_memory_layout(views,1,&small,blks,0,&small_blocks,&g2) && small==nv && small_blocks==nb);
        const unsigned char *backing=runtime_memory_backing(&direct,&total);
        assert(backing && direct==POOL_SIZE && total>direct);
        int found_a=0, found_flex=0, found_block=0;
        ((unsigned char *)a)[5]=0xA5; ((unsigned char *)flex)[PAGE+7]=0x5A;
        for (size_t i=0;i<nv;++i) {
            assert(i==0 || views[i-1].end<=views[i].start);
            if (views[i].start==(uintptr_t)a) {
                found_a=1;
                assert(!views[i].flexible && views[i].phys==(uint64_t)physical && views[i].end==(uintptr_t)a+2*PAGE);
                assert(backing[views[i].phys+5]==0xA5);
            }
            if (views[i].start==(uintptr_t)flex) {
                found_flex=1;
                assert(views[i].flexible && views[i].phys>=direct && views[i].end==(uintptr_t)flex+3*PAGE);
                assert(backing[views[i].phys+PAGE+7]==0x5A);
            }
        }
        for (size_t i=0;i<nb;++i) {
            assert(i==0 || blks[i-1].start<blks[i].start);
            found_block|=blks[i].start==(uint64_t)physical && blks[i].size==4*PAGE;
        }
        assert(found_a && found_flex && found_block);
        /* The capture keeps backing pages by VM disposition: a page written only through a guest
         * view must show as present through the backing view; a never-touched one must not. */
        int disp[2]={-1,-1};
        mach_vm_size_t one=1;
        assert(mach_vm_page_range_query(mach_task_self(),(mach_vm_address_t)(backing+physical),vm_page_size,
                                        (mach_vm_address_t)&disp[0],&one)==KERN_SUCCESS && one==1);
        one=1;
        assert(mach_vm_page_range_query(mach_task_self(),(mach_vm_address_t)(backing+physical+3*PAGE),vm_page_size,
                                        (mach_vm_address_t)&disp[1],&one)==KERN_SUCCESS && one==1);
        printf("  backing dispositions: written page %#x, untouched page %#x\n",disp[0],disp[1]);
        assert(disp[0]!=0 && disp[1]==0);
        printf("  layout: %zu views, %zu direct blocks; backing reads match guest writes\n",nv,nb);
    }
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

    /* Guest-heap object identity (perf replay host resets): interior lookups name the object,
     * ordinals follow allocation order, a freed object and a non-object address have none. */
    {
        unsigned char *x=runtime_guest_malloc(64), *y=runtime_guest_malloc(100);
        RuntimeGuestObject ox, oy, none;
        assert(x && y);
        assert(!runtime_guest_object((uintptr_t)x+10,&ox) && ox.start==(uintptr_t)x && ox.size>=64);
        assert(!runtime_guest_object((uintptr_t)y,&oy) && oy.start==(uintptr_t)y && oy.ordinal==ox.ordinal+1);
        assert(strcmp(ox.module,"unknown") && ox.offset);
        assert(runtime_guest_object((uintptr_t)y+oy.size,&none)==-1 || none.start!=(uintptr_t)y);
        runtime_guest_free(x);
        assert(runtime_guest_object((uintptr_t)x+10,&none)==-1);
        assert(runtime_guest_object((uintptr_t)&none,&none)==-1); /* a stack address */
        runtime_guest_free(y);
        printf("  guest object identity: ordinal %llu, allocated at %s+%#llx\n",
               (unsigned long long)ox.ordinal,ox.module,(unsigned long long)ox.offset);
    }
    /* Replay map preflight: a malformed view table fails before anything is mapped. */
    {
        char why[512];
        const uint64_t at=USER_MIN+UINT64_C(0x100000000);
        ReplayBlock blk[2]={{0,4*PAGE,0,0},{8*PAGE,4*PAGE,0,0}};
        ReplayView ok[2]={{at,at+2*PAGE,0,0,3,0,0},{at+4*PAGE,at+6*PAGE,8*PAGE,0,3,0,0}};
        if (replay_preflight(blk,2,ok,2,why,sizeof(why))) { printf("  valid table refused: %s\n",why); assert(0); }
        ReplayView v[2];
        memcpy(v,ok,sizeof(v)); v[1].start=at+PAGE;                      /* overlap */
        assert(replay_preflight(blk,2,v,2,why,sizeof(why))==-1);
        memcpy(v,ok,sizeof(v)); v[0].start=LOW_MIN; v[0].end=LOW_MIN+PAGE; /* host low arena */
        assert(replay_preflight(blk,2,v,2,why,sizeof(why))==-1);
        memcpy(v,ok,sizeof(v)); v[0].end=at+PAGE+1;                       /* misaligned */
        assert(replay_preflight(blk,2,v,2,why,sizeof(why))==-1);
        memcpy(v,ok,sizeof(v)); v[1].phys=5*PAGE;                         /* backed by no block */
        assert(replay_preflight(blk,2,v,2,why,sizeof(why))==-1);
        memcpy(v,ok,sizeof(v)); v[0].flexible=2;                          /* unknown kind */
        assert(replay_preflight(blk,2,v,2,why,sizeof(why))==-1);
        ReplayBlock bad[2]={{0,4*PAGE,0,0},{2*PAGE,4*PAGE,0,0}};          /* overlapping blocks */
        assert(replay_preflight(bad,2,ok,2,why,sizeof(why))==-1);
        void *live=NULL;                                                  /* over a live mapping */
        assert(!map_flexible(&live,PAGE,3,0));
        ReplayView over={(uint64_t)(uintptr_t)live,(uint64_t)(uintptr_t)live+PAGE,POOL_SIZE,1,3,0,0};
        assert(replay_preflight(blk,2,&over,1,why,sizeof(why))==-1);
        printf("  replay preflight: last refusal: %s\n",why);
        assert(!release_flexible(live,PAGE));
        puts("PASS: guest object identity and replay map preflight controls");
    }
}
