/* Standalone SDK-only macOS memory contracts; run with tests/test_memory_macos.sh. */
#include "runtime.h"
#include <assert.h>
#include <limits.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#define PAGE UINT64_C(16384)
#define GUEST_MIN UINT64_C(0x7000000000)
#define GUEST_MAX UINT64_C(0xfc00000000)
#define FIXED 0x10
#define GET(type,name) ((type)runtime_memory_resolve(name))
typedef int32_t (ABI *Allocate)(int64_t,int64_t,uint64_t,uint64_t,int,int64_t *);
typedef int32_t (ABI *Map)(void **,uint64_t,int,int,int64_t,uint64_t);
typedef int32_t (ABI *Release)(uint64_t,uint64_t);
typedef int32_t (ABI *Unmap)(void *,uint64_t);
typedef int32_t (ABI *Flexible)(void **,uint64_t,int,int);
typedef int32_t (ABI *Reserve)(void **,uint64_t,int,uint64_t);
typedef int32_t (ABI *Protect)(const void *,uint64_t,int);
int runtime_memory_write_backing(uintptr_t,const void *,uint64_t);
void runtime_memory_gpu_protect(uintptr_t,uint64_t,int,int);
uintptr_t runtime_lookup(const RuntimeExport *table,size_t count,const char *name) {
    for (size_t i=0;i<count;++i) if (!strcmp(table[i].name,name)) return (uintptr_t)table[i].function;
    return 0;
}
static volatile uint64_t *watched;
static int stop_reader, reader_ready, torn;
/* Reads a label the writer alternates between all-zero and all-one bits. */
static void *read_labels(void *unused) {
    (void)unused;
    __atomic_store_n(&reader_ready,1,__ATOMIC_RELEASE);
    while (!__atomic_load_n(&stop_reader,__ATOMIC_ACQUIRE)) {
        const uint64_t v=__atomic_load_n(watched,__ATOMIC_ACQUIRE);
        if (v!=0 && v!=UINT64_MAX) torn=1;
    }
    return NULL;
}
static vm_prot_t protection(const void *p) {
    mach_vm_address_t address=(mach_vm_address_t)p;
    mach_vm_size_t size;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t count=VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object;
    assert(mach_vm_region(mach_task_self(),&address,&size,VM_REGION_BASIC_INFO_64,
                         (vm_region_info_t)&info,&count,&object)==KERN_SUCCESS);
    if (object!=MACH_PORT_NULL) mach_port_deallocate(mach_task_self(),object);
    assert(address<=(uintptr_t)p && (uintptr_t)p<address+size);
    return info.protection;
}
static void *allocator_worker(void *unused) {
    (void)unused;
    for (unsigned round=0;round<100;++round) {
        void *objects[32];
        for (unsigned i=0;i<32;++i) {
            objects[i]=runtime_guest_calloc(i+1,31);
            assert(objects[i] && (uintptr_t)objects[i]<UINT64_C(1)<<40);
            assert(!((uintptr_t)objects[i]&63));
            for (unsigned j=0;j<(i+1)*31;++j) assert(!((unsigned char *)objects[i])[j]);
            memset(objects[i],0x77,(i+1)*31);
        }
        for (unsigned i=0;i<32;++i) runtime_guest_free(objects[i]);
    }
    return NULL;
}
static void low_objects(void) {
    assert(!runtime_low_map(0,PROT_READ));
    assert(!runtime_low_map(SIZE_MAX,PROT_READ));
    assert(!runtime_guest_malloc(SIZE_MAX));
    assert(!runtime_guest_calloc(SIZE_MAX,2));
    void *a=runtime_guest_malloc(128), *b=runtime_guest_malloc(128);
    assert(a && b);
    runtime_guest_free(a); runtime_guest_free(b);
    void *merged=runtime_guest_malloc(320);
    assert(merged==a);
    runtime_guest_free(merged);
    pthread_t workers[4];
    for (unsigned i=0;i<4;++i) assert(!pthread_create(&workers[i],NULL,allocator_worker,NULL));
    for (unsigned i=0;i<4;++i) assert(!pthread_join(workers[i],NULL));
    puts("PASS: low objects, alignment, overflow, zeroing, coalescing, concurrent reuse");
}
int main(void) {
    runtime_memory_reserve(); runtime_memory_reserve();
    low_objects();
    Allocate allocate=GET(Allocate,"sceKernelAllocateDirectMemory");
    Map map=GET(Map,"sceKernelMapDirectMemory");
    Release release=GET(Release,"sceKernelReleaseDirectMemory");
    Unmap unmap=GET(Unmap,"sceKernelMunmap");
    Flexible flexible=GET(Flexible,"sceKernelMapFlexibleMemory");
    Reserve reserve=GET(Reserve,"sceKernelReserveVirtualRange");
    Protect protect=GET(Protect,"sceKernelMprotect");
    int64_t physical=-1;
    assert(!allocate(0,INT64_MAX,5*PAGE,PAGE,0,&physical));
    void *a=NULL,*b=NULL;
    assert(!map(&a,5*PAGE,3,0,physical,PAGE));
    assert((uintptr_t)a>=GUEST_MIN && (uintptr_t)a+5*PAGE<=GUEST_MAX);
    assert(!map(&b,5*PAGE,3,0,physical,PAGE));
    memset(a,0x55,5*PAGE);
    assert(((unsigned char *)b)[3*PAGE]==0x55);
    assert(!protect(a,PAGE,1));
    runtime_memory_gpu_protect((uintptr_t)a,PAGE,1,0);
    assert(protection(a)==VM_PROT_READ);
    unsigned char byte=0x42;
    assert(runtime_memory_write_backing((uintptr_t)a,&byte,1));
    assert(*(unsigned char *)a==byte && *(unsigned char *)b==byte);
    assert(!release(physical+2*PAGE,PAGE));
    assert(protection((char *)a+2*PAGE)==VM_PROT_NONE);
    assert(protection((char *)b+2*PAGE)==VM_PROT_NONE);
    assert(((unsigned char *)a)[PAGE]==0x55 && ((unsigned char *)b)[3*PAGE]==0x55);
    int64_t recycled=-1;
    assert(!allocate(physical+2*PAGE,physical+3*PAGE,PAGE,PAGE,0,&recycled));
    assert((uint64_t)recycled==(uint64_t)physical+2*PAGE);
    void *whole=NULL;
    assert(!map(&whole,5*PAGE,3,0,physical,PAGE));
    for (size_t i=0;i<PAGE;++i) assert(!((unsigned char *)whole)[2*PAGE+i]);
    assert(((unsigned char *)whole)[PAGE]==0x55 && ((unsigned char *)whole)[3*PAGE]==0x55);
    ((unsigned char *)whole)[3*PAGE]=0x66;
    assert(((unsigned char *)b)[3*PAGE]==0x66);
    assert(!release(physical,5*PAGE));
    assert(protection(a)==VM_PROT_NONE && protection(whole)==VM_PROT_NONE);
    puts("PASS: coherent aliases, protected backing writes, partial release, sparse zero reuse");

    void *region=NULL;
    assert(!reserve(&region,3*PAGE,0,PAGE));
    assert(protection(region)==VM_PROT_NONE);
    void *middle=(char *)region+PAGE;
    assert(!allocate(0,INT64_MAX,PAGE,PAGE,0,&physical));
    assert(!map(&middle,PAGE,3,FIXED,physical,PAGE));
    assert(protection(region)==VM_PROT_NONE);
    assert(protection(middle)==(VM_PROT_READ|VM_PROT_WRITE));
    assert(protection((char *)region+2*PAGE)==VM_PROT_NONE);
    void *bad=(void *)UINT64_C(0x1000000000);
    assert(map(&bad,PAGE,3,FIXED,physical,PAGE)!=0);
    bad=(void *)(GUEST_MAX-PAGE);
    assert(map(&bad,2*PAGE,3,FIXED,physical,PAGE)!=0);
    assert(!release(physical,PAGE));
    assert(!unmap(region,3*PAGE));
    puts("PASS: reserved fixed views, preserved holes, forbidden addresses rejected");

    void *flex=NULL;
    assert(!flexible(&flex,3*PAGE,3,0));
    memset(flex,0x31,3*PAGE);
    assert(!unmap((char *)flex+PAGE,PAGE));
    unsigned char changed=0x19;
    assert(runtime_memory_write_backing((uintptr_t)flex+2*PAGE,&changed,1));
    assert(((unsigned char *)flex)[2*PAGE]==changed && *(unsigned char *)flex==0x31);
    void *flex2=NULL;
    assert(!flexible(&flex2,PAGE,3,0));
    for (size_t i=0;i<PAGE;++i) assert(!((unsigned char *)flex2)[i]);
    assert(!unmap(flex,3*PAGE));
    assert(!unmap(flex2,PAGE));
    puts("PASS: flexible partial-unmap offsets and zero reuse");

    assert(!allocate(0,INT64_MAX,PAGE,PAGE,0,&physical));
    void *code=NULL;
    assert(!map(&code,PAGE,3,0,physical,PAGE));
    const unsigned char ret42[]={0xb8,42,0,0,0,0xc3};
    memcpy(code,ret42,sizeof(ret42));
    assert(!protect(code,PAGE,5));
    assert(((int (*)(void))code)()==42);
    byte=17;
    assert(runtime_memory_write_backing((uintptr_t)code+1,&byte,1));
    assert(((int (*)(void))code)()==17);
    void *data_alias=NULL;
    assert(!map(&data_alias,PAGE,3,0,physical,PAGE));
    byte=23;
    assert(runtime_memory_write_backing((uintptr_t)data_alias+1,&byte,1));
    assert(((int (*)(void))code)()==23);
    assert(protection(code)==(VM_PROT_READ|VM_PROT_EXECUTE));
    assert(!release(physical,PAGE));
    puts("PASS: executable shared views and automatic Rosetta translation invalidation across aliases");

    // GPU labels: an aligned 2/4/8-byte backing write is one store. A reader of the guest view
    // racing a writer that alternates two values sees no torn value. (Darwin's memcpy did not
    // tear here either; this guards the guarantee, it does not reproduce a failure.)
    assert(!allocate(0,INT64_MAX,PAGE,PAGE,0,&physical));
    void *labels=NULL;
    assert(!map(&labels,PAGE,3,0,physical,PAGE));
    memset(labels,0xee,64);
    const uint64_t q=0x1122334455667788ull; const uint32_t d=0x99aabbcc; const uint16_t h=0xddee;
    assert(runtime_memory_write_backing((uintptr_t)labels+8,&q,8));
    assert(runtime_memory_write_backing((uintptr_t)labels+20,&d,4));
    assert(runtime_memory_write_backing((uintptr_t)labels+26,&h,2));
    assert(runtime_memory_write_backing((uintptr_t)labels+33,&q,8)); // unaligned: plain copy
    assert(!memcmp((char *)labels+8,&q,8) && !memcmp((char *)labels+20,&d,4) &&
           !memcmp((char *)labels+26,&h,2) && !memcmp((char *)labels+33,&q,8));
    assert(((unsigned char *)labels)[7]==0xee && ((unsigned char *)labels)[16]==0xee &&
           ((unsigned char *)labels)[24]==0xee && ((unsigned char *)labels)[28]==0xee);
    watched=(volatile uint64_t *)labels;
    const uint64_t zero=0;
    assert(runtime_memory_write_backing((uintptr_t)labels,&zero,8)); // a value the reader accepts
    pthread_t reader;
    assert(!pthread_create(&reader,NULL,read_labels,NULL));
    while (!__atomic_load_n(&reader_ready,__ATOMIC_ACQUIRE)) sched_yield(); // the race runs
    const uint64_t values[2]={0,UINT64_MAX};
    for (int i=0;i<2000000;++i) assert(runtime_memory_write_backing((uintptr_t)labels,&values[i&1],8));
    __atomic_store_n(&stop_reader,1,__ATOMIC_RELEASE);
    assert(!pthread_join(reader,NULL));
    assert(!torn);
    puts("PASS: aligned 2/4/8-byte backing writes land whole (no torn value seen); unaligned copy");
    runtime_memory_report();
    return 0;
}
