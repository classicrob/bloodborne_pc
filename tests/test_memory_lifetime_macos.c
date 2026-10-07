/* SDK-only checks of the actual backend's imported GPU memory lifetime order. */
#include "../src/runtime_memory.c"
#include <assert.h>
#include <signal.h>
uintptr_t runtime_lookup(const RuntimeExport *table,size_t count,const char *name) {
    for (size_t i=0;i<count;++i) if (!strcmp(table[i].name,name)) return (uintptr_t)table[i].function;
    return 0;
}
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static unsigned begins,ends,retires,notifications;
static int active;
typedef struct { uintptr_t address; uint64_t size; unsigned char value; int live; } Import;
static Import imports[16];
static void track(void *address,uint64_t size,unsigned char value) {
    for (unsigned i=0;i<16;++i) if (!imports[i].live) {
        imports[i]=(Import){(uintptr_t)address,size,value,1}; return;
    }
    assert(0);
}
static unsigned live(void) {
    unsigned n=0; for(unsigned i=0;i<16;++i) n+=imports[i].live; return n;
}
static void *reader(void *unused) {
    (void)unused; read_lock(); read_unlock(); return NULL;
}
static void begin(void) {
    assert(!exclusive_depth);
    assert(!pthread_mutex_lock(&gate)); assert(!active); active=1; ++begins;
    /* Joining this reader would deadlock if begin ran with the VMA writer held. */
    pthread_t worker; assert(!pthread_create(&worker,NULL,reader,NULL));
    assert(!pthread_join(worker,NULL));
}
static void retire(uintptr_t address,uint64_t size) {
    assert(active && exclusive_depth && lifetime_depth); ++retires;
    for(unsigned i=0;i<16;++i) {
        Import *p=&imports[i];
        if (!p->live || p->address>=address+size || address>=p->address+p->size) continue;
        /* The old mapping must still be readable and contain the old data. */
        assert(*(unsigned char *)p->address==p->value);
        p->live=0;
    }
}
static void notify(uintptr_t address,uint64_t size) {
    (void)address; (void)size;
    assert(active && !exclusive_depth && lifetime_depth); ++notifications;
}
static void end(void) {
    assert(active && !exclusive_depth && !lifetime_depth && !pending_count);
    active=0; ++ends; assert(begins==ends); assert(!pthread_mutex_unlock(&gate));
}
static void balanced(void) { assert(!active && begins==ends && !lifetime_depth); }
int main(void) {
    alarm(30);
    runtime_memory_reserve();
    runtime_memory_set_gpu_hooks(notify,notify,notify);
    runtime_memory_set_gpu_lifetime_hooks(begin,retire,end);
    int64_t physical;
    assert(!direct_allocate(0,POOL_SIZE,4*PAGE,PAGE,0,&physical));
    void *a=NULL,*b=NULL;
    assert(!direct_map(&a,4*PAGE,3,0,physical,PAGE));
    assert(!direct_map(&b,4*PAGE,3,0,physical,PAGE));
    uintptr_t region_start,region_end; uint64_t offset;
    assert(runtime_memory_backing_region((uintptr_t)a+PAGE,&region_start,&region_end,&offset));
    assert(region_start==(uintptr_t)a && region_end==(uintptr_t)a+4*PAGE && offset==(uint64_t)physical);
    assert(runtime_memory_backing_region((uintptr_t)b,&region_start,&region_end,&offset));
    assert(region_start==(uintptr_t)b && offset==(uint64_t)physical);
    assert(!runtime_memory_backing_region(LOW_MIN,&region_start,&region_end,&offset));
    memset(a,0x5a,4*PAGE); track(a,4*PAGE,0x5a); track(b,4*PAGE,0x5a);
    assert(!direct_release(physical+PAGE,PAGE)); assert(!live()); balanced();
    assert(!runtime_memory_backing_region((uintptr_t)a+PAGE,&region_start,&region_end,&offset));
    assert(runtime_memory_backing_region((uintptr_t)b+2*PAGE,&region_start,&region_end,&offset));
    assert(region_start==(uintptr_t)b+2*PAGE && offset==(uint64_t)physical+2*PAGE);
    assert(((unsigned char *)a)[0]==0x5a && ((unsigned char *)b)[2*PAGE]==0x5a);
    track(a,PAGE,0x5a); track(b,PAGE,0x5a);
    assert(!direct_release(physical,PAGE)); assert(!live());
    assert(!direct_release(physical+2*PAGE,2*PAGE));
    assert(!direct_allocate(physical,physical+4*PAGE,4*PAGE,PAGE,0,&physical));
    a=NULL; assert(!direct_map(&a,4*PAGE,3,0,physical,PAGE));
    for(unsigned i=0;i<4*PAGE;++i) assert(!((unsigned char *)a)[i]);
    track(a,4*PAGE,0);
    assert(!kernel_mprotect(a,PAGE,0)); assert(!live());
    assert(!kernel_mprotect(a,PAGE,3));
    track(a,4*PAGE,0);
    void *replacement=a;
    assert(!map_flexible(&replacement,4*PAGE,3,MAP_FIXED_FLAG)); assert(!live());
    assert(runtime_memory_backing_region((uintptr_t)a,&region_start,&region_end,&offset));
    assert(offset>=POOL_SIZE && region_end-region_start==4*PAGE);
    memset(a,0x6b,4*PAGE); track(a,4*PAGE,0x6b);
    assert(!direct_unmap(a,4*PAGE)); assert(!live());
    assert(!direct_release(physical,4*PAGE));
    void *flex=NULL; assert(!map_flexible(&flex,2*PAGE,3,0));
    memset(flex,0x7c,2*PAGE); track(flex,2*PAGE,0x7c);
    unsigned before=begins;
    BatchEntry batch[]={{.start=flex,.length=PAGE,.operation=1},
        {.start=(unsigned char *)flex+PAGE,.length=PAGE,.prot=0,.operation=2},
        {.operation=99}};
    int done=-1;
    assert(batch_map2(batch,3,&done,0)==INVALID && done==2);
    assert(begins==before+1 && !live()); balanced();
    assert(!direct_unmap((unsigned char *)flex+PAGE,PAGE));
    before=begins;
    assert(direct_map(NULL,PAGE,3,0,0,PAGE)==INVALID);
    assert(direct_release(physical,4*PAGE)==INVALID);
    assert(kernel_mprotect((void *)USER_MIN,PAGE,3)==INVALID);
    assert(begins==before+3); balanced();
    void *reserved=NULL; assert(!reserve_range(&reserved,PAGE,0,PAGE));
    assert(!runtime_memory_backing_region((uintptr_t)reserved,&region_start,&region_end,&offset));
    before=begins;
    assert(reserve_range(&reserved,PAGE,MAP_FIXED_FLAG,PAGE)==NO_MEMORY);
    assert(begins==before+1); balanced();
    assert(!direct_unmap(reserved,PAGE));
    assert(retires>=6 && notifications>=10);
    runtime_memory_set_gpu_lifetime_hooks(NULL,NULL,NULL);
    printf("PASS: GPU lifetime order, alias/partial release, fixed replacement, flex, protection, batch, failures (%u transactions)\n",ends);
}
