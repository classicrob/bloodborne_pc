/* Contention controls for the guest GFX heap lock and pthread mutex mappings. */
#include "../src/runtime_mutex.c"
#include <assert.h>
#include <signal.h>
#include <stdatomic.h>
struct GuestThread;
struct GuestThread *runtime_thread_current(void);
#include <unistd.h>
#include "test_alarm.h"

uintptr_t runtime_lookup(const RuntimeExport *table,size_t count,const char *name) {
    for (size_t i=0;i<count;++i) if (!strcmp(table[i].name,name)) return (uintptr_t)table[i].function;
    return 0;
}
void runtime_thread_keys_cleanup(void) {}
typedef int32_t (ABI *Create)(uint32_t *,const char *,uint32_t,int32_t,int32_t,const void *);
typedef int32_t (ABI *Count)(uint32_t,int32_t);
typedef int32_t (ABI *Wait)(uint32_t,int32_t,uint32_t *);
typedef int32_t (ABI *Delete)(uint32_t);
static Create create_sem;
static Count signal_sem;
static Wait wait_sem;
static Delete delete_sem;
#define OWNER_MASK UINT64_C(0xffffffffff)
#define WAITER_ONE UINT64_C(0x10000000000)
static _Atomic uint64_t adaptive;
static _Atomic unsigned slow_waits, inside;
static uint32_t semaphore;
static GuestMutex *test_mutex;
static int mode;
static pthread_barrier_t start_barrier;
static uint64_t counter;
static unsigned next_node[32], prev_node[32];
static uintptr_t identities[8];

/* Same owner/waiter state transitions as eboot 20368c0/2036a40.
 * Spin budget is varied by the fixture rather than read from RDTSC. */
static void adaptive_acquire(uintptr_t owner,unsigned spins) {
    assert(owner && owner<OWNER_MASK);
    for (;;) {
        uint64_t old=atomic_load(&adaptive), value;
        if (!(old&OWNER_MASK) || (old&OWNER_MASK)==owner)
            value=(old&~OWNER_MASK)|owner;
        else value=spins ? old : old+WAITER_ONE;
        if (!atomic_compare_exchange_weak(&adaptive,&old,value)) continue;
        if ((value&OWNER_MASK)==owner) return;
        if (spins) { --spins; continue; }
        atomic_fetch_add(&slow_waits,1);
        assert(wait_sem(semaphore,1,NULL)==0);
        return;
    }
}
static void adaptive_release(void) {
    uint64_t old=atomic_load(&adaptive), value;
    do {
        value=(old>>40) ? (old-WAITER_ONE)|OWNER_MASK : old&~OWNER_MASK;
    } while (!atomic_compare_exchange_weak(&adaptive,&old,value));
    if (value&OWNER_MASK) assert(signal_sem(semaphore,1)==0);
}
static void enter(uintptr_t owner,unsigned iteration) {
    if (!mode) adaptive_acquire(owner,iteration%3 ? 0 : 64);
    else if (iteration%3==0) assert(mutex_lock(&test_mutex)==0);
    else if (iteration%3==1) {
        int32_t result=mutex_trylock(&test_mutex);
        assert(!result || (uint32_t)result==0x80020010);
        if (result) assert(mutex_lock(&test_mutex)==0);
    } else assert(mutex_timedlock(&test_mutex,5000000)==0);
}
static void leave(void) {
    if (!mode) adaptive_release(); else assert(mutex_unlock(&test_mutex)==0);
}
static void *worker(void *arg) {
    unsigned id=(unsigned)(uintptr_t)arg;
    uintptr_t owner=(uintptr_t)runtime_thread_current(); identities[id]=owner;
    assert(owner && owner<OWNER_MASK);
    int b=pthread_barrier_wait(&start_barrier); assert(!b || b==PTHREAD_BARRIER_SERIAL_THREAD);
    for (unsigned i=0;i<4000;++i) {
        enter(owner,i);
        assert(atomic_fetch_add(&inside,1)==0);
        for (unsigned n=0;n<32;++n) assert(prev_node[next_node[n]]==n && next_node[prev_node[n]]==n);
        unsigned n=1+(unsigned)(counter%31), p=prev_node[n], q=next_node[n];
        next_node[p]=q; prev_node[q]=p;
        q=next_node[0]; next_node[0]=n; prev_node[n]=0; next_node[n]=q; prev_node[q]=n;
        ++counter;
        if (!(i%64)) sched_yield();
        assert(atomic_fetch_sub(&inside,1)==1);
        leave();
    }
    return NULL;
}
static void contention(int type) {
    mode=type; counter=0; atomic_store(&adaptive,0); atomic_store(&inside,0);
    for(unsigned n=0;n<32;++n) {next_node[n]=(n+1)%32; prev_node[n]=(n+31)%32;}
    GuestAttr *attr=NULL;
    if (type) {
        assert(!attr_init(&attr) && !attr_type(&attr,type));
        assert(!mutex_init(&test_mutex,&attr,"contention")); assert(!attr_destroy(&attr));
    } else assert(create_sem(&semaphore,"adaptive-heap",2,0,1,NULL)==0);
    assert(!pthread_barrier_init(&start_barrier,NULL,8));
    pthread_t threads[8];
    for (uintptr_t i=0;i<8;++i) assert(!pthread_create(&threads[i],NULL,worker,(void *)i));
    for (unsigned i=0;i<8;++i) assert(!pthread_join(threads[i],NULL));
    for (unsigned i=0;i<8;++i) for(unsigned j=0;j<i;++j) assert(identities[i]!=identities[j]);
    assert(counter==32000 && !atomic_load(&inside));
    assert(!pthread_barrier_destroy(&start_barrier));
    if (type) assert(!mutex_destroy(&test_mutex));
    else { assert(atomic_load(&slow_waits)>0 && !atomic_load(&adaptive)); assert(!delete_sem(semaphore)); }
    printf("PASS: lock type %d, 8 workers, 32000 protected list transactions\n",type);
}
static void *nonowner_unlock(void *unused) { (void)unused; return (void *)(uintptr_t)(uint32_t)mutex_unlock(&test_mutex); }
static void ownership(int type) {
    GuestAttr *attr=NULL; assert(!attr_init(&attr) && !attr_type(&attr,type));
    assert(!mutex_init(&test_mutex,&attr,"owner")); assert(!attr_destroy(&attr));
    assert(!mutex_lock(&test_mutex));
    if (type!=3) {
        pthread_t t; void *result; assert(!pthread_create(&t,NULL,nonowner_unlock,NULL));
        assert(!pthread_join(t,&result)); assert((uintptr_t)result==0x80020001);
    }
    if (type==1 || type==4) assert((uint32_t)mutex_lock(&test_mutex)==0x8002000b);
    if (type!=2) assert((uint32_t)mutex_timedlock(&test_mutex,1000)==0x8002003c);
    int32_t result=mutex_trylock(&test_mutex);
    if (type==2) { assert(!result); assert(!mutex_unlock(&test_mutex)); }
    else assert((uint32_t)result==0x80020010);
    assert(!mutex_unlock(&test_mutex)); assert(!mutex_destroy(&test_mutex));
}
typedef int32_t (ABI *Cancel)(uint32_t,int32_t,int32_t *);
typedef struct { uint32_t id, timeout; int32_t result; _Atomic int done; } Pending;
static void *pending_wait(void *arg) {
    Pending *p=arg; p->result=wait_sem(p->id,1,&p->timeout); atomic_store(&p->done,1); return NULL;
}
static void enrolled(uint32_t id,unsigned count) {
    for (unsigned i=0;i<5000;++i) {
        if (runtime_sema_waiters(id)==count) return;
        struct timespec nap={0,1000000}; nanosleep(&nap,NULL);
    }
    assert(!"semaphore enrollment timeout");
}
static void semaphore_edges(void) {
    Count poll_sem=(Count)runtime_sema_resolve("12wOHk8ywb0#p#J");
    Cancel cancel_sem=(Cancel)runtime_sema_resolve("4DM06U2BNEY#p#J");
    uint32_t id; assert(!create_sem(&id,"heap-edges",2,0,1,NULL));
    assert(!signal_sem(id,1));
    assert(signal_sem(id,1)!=0); /* Overflow must not manufacture another token. */
    assert(!poll_sem(id,1) && (uint32_t)poll_sem(id,1)==0x80020010);
    uint32_t timeout=1000; assert((uint32_t)wait_sem(id,1,&timeout)==0x8002003c);
    assert(!timeout && !runtime_sema_waiters(id));
    assert(!signal_sem(id,1) && !poll_sem(id,1));
    Pending p[8]; pthread_t threads[8];
    for (unsigned i=0;i<8;++i) {
        p[i]=(Pending){.id=id,.timeout=5000000};
        assert(!pthread_create(&threads[i],NULL,pending_wait,&p[i])); enrolled(id,i+1);
    }
    for (unsigned i=0;i<8;++i) {
        assert(!signal_sem(id,1));
        assert(!pthread_join(threads[i],NULL) && !p[i].result);
        assert(runtime_sema_waiters(id)==7-i);
        assert((uint32_t)poll_sem(id,1)==0x80020010);
        for (unsigned j=i+1;j<8;++j) assert(!atomic_load(&p[j].done));
    }
    for (unsigned i=0;i<8;++i) {
        p[i]=(Pending){.id=id,.timeout=5000000};
        assert(!pthread_create(&threads[i],NULL,pending_wait,&p[i])); enrolled(id,i+1);
    }
    int32_t cancelled=-1; assert(!cancel_sem(id,0,&cancelled) && cancelled==8);
    for (unsigned i=0;i<8;++i) {
        assert(!pthread_join(threads[i],NULL));
        assert((uint32_t)p[i].result==0x80020055 && !p[i].timeout);
    }
    assert(!runtime_sema_waiters(id) && (uint32_t)poll_sem(id,1)==0x80020010);
    assert(!signal_sem(id,1) && !poll_sem(id,1) && !delete_sem(id));
    puts("PASS: max-one overflow, timeout, single-token wake, FIFO, cancel, reuse");
}
int main(void) {
    test_alarm(60);
    create_sem=(Create)runtime_sema_resolve("188x57JYp0g#p#J");
    signal_sem=(Count)runtime_sema_resolve("4czppHBiriw#p#J");
    wait_sem=(Wait)runtime_sema_resolve("Zxa0VhQVTsk#p#J");
    delete_sem=(Delete)runtime_sema_resolve("R1Jvn8bSCW8#p#J");
    semaphore_edges();
    contention(0);
    for(int type=1;type<=4;++type) { contention(type); ownership(type); }
    printf("PASS: adaptive slow handoffs=%u; native ownership/recursion controls\n",atomic_load(&slow_waits));
    return 0;
}
