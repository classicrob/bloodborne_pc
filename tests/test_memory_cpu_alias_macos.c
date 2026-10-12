/* CPU command-stream visibility through actual runtime and import-style aliases. */
#include "../src/runtime_memory.c"
#include <assert.h>
#include <signal.h>
#include "test_alarm.h"
uintptr_t runtime_lookup(const RuntimeExport *table,size_t count,const char *name) {
    for (size_t i=0;i<count;++i) if (!strcmp(table[i].name,name)) return (uintptr_t)table[i].function;
    return 0;
}
#define WORDS 1024
#define ROUNDS 3000
static pthread_mutex_t sync_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sync_cond=PTHREAD_COND_INITIALIZER;
static unsigned ready,consumed;
static uint32_t *guest,*alias,*mirror,*backing;
static uint32_t value(unsigned round,unsigned word) { return (round*UINT32_C(2654435761)) ^ (word*17+3); }
static void *consume(void *unused) {
    (void)unused;
    assert(!pthread_mutex_lock(&sync_lock));
    for (unsigned round=1;round<=ROUNDS;++round) {
        while (ready<round) assert(!pthread_cond_wait(&sync_cond,&sync_lock));
        for (unsigned word=0;word<WORDS;++word) {
            uint32_t expected=value(round,word);
            assert(guest[word]==expected);
            assert(alias[word]==expected);
            assert(mirror[word]==expected);
            assert(backing[word]==expected);
        }
        consumed=round;
        assert(!pthread_cond_broadcast(&sync_cond));
    }
    assert(!pthread_mutex_unlock(&sync_lock));
    return NULL;
}
int main(void) {
    test_alarm(30);
    puts("CPU alias visibility: actual Mach backend, synchronized producer/consumer"); fflush(stdout);
    int64_t physical;
    assert(!direct_allocate(0,POOL_SIZE,PAGE,PAGE,0,&physical));
    void *a=NULL,*b=NULL;
    assert(!direct_map(&a,PAGE,3,0,physical,PAGE));
    assert(!direct_map(&b,PAGE,3,0,physical,PAGE));
    guest=a; alias=b; backing=(uint32_t *)(backing_base+physical);
    runtime_memory_gpu_protect((uintptr_t)guest,PAGE,1,0);
    mach_vm_address_t mirror_address=0;
    vm_prot_t current,maximum;
    assert(mach_vm_remap(mach_task_self(),&mirror_address,PAGE,0,VM_FLAGS_ANYWHERE,
        mach_task_self(),(mach_vm_address_t)guest,FALSE,&current,&maximum,VM_INHERIT_NONE)==KERN_SUCCESS);
    assert(mach_vm_protect(mach_task_self(),mirror_address,PAGE,FALSE,
        VM_PROT_READ|VM_PROT_WRITE)==KERN_SUCCESS);
    mirror=(uint32_t *)mirror_address;
    pthread_t thread;
    assert(!pthread_create(&thread,NULL,consume,NULL));
    assert(!pthread_mutex_lock(&sync_lock));
    for (unsigned round=1;round<=ROUNDS;++round) {
        uint32_t *writer=round%3==0 ? guest : round%3==1 ? backing : mirror;
        if (writer==guest) runtime_memory_gpu_protect((uintptr_t)guest,PAGE,1,1);
        for (unsigned word=0;word<WORDS;++word) writer[word]=value(round,word);
        runtime_memory_gpu_protect((uintptr_t)guest,PAGE,1,0);
        ready=round;
        assert(!pthread_cond_broadcast(&sync_cond));
        while (consumed<round) assert(!pthread_cond_wait(&sync_cond,&sync_lock));
    }
    assert(!pthread_mutex_unlock(&sync_lock));
    assert(!pthread_join(thread,NULL));
    assert(mach_vm_deallocate(mach_task_self(),mirror_address,PAGE)==KERN_SUCCESS);
    assert(!direct_release(physical,PAGE));
    printf("PASS: %u published buffers, %u words each, four CPU views; guest/backing/mirror writes and RO protection cycles\n",ROUNDS,WORDS);
}
