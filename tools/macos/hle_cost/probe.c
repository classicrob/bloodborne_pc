#define _GNU_SOURCE
#include "runtime.h"
#include <assert.h>
#include <inttypes.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/mach_vm.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>

void runtime_restart(void) { abort(); }
typedef uintptr_t (ABI *Call)(uintptr_t,uintptr_t,uintptr_t,uintptr_t);
static volatile uintptr_t sink;
static mach_timebase_info_data_t timebase;
static uint64_t wall_ns(void) {
    return (uint64_t)((__uint128_t)mach_absolute_time()*timebase.numer/timebase.denom);
}
static uint64_t cpu_ns(void) {
    struct rusage r; assert(!getrusage(RUSAGE_SELF,&r));
    return ((uint64_t)r.ru_utime.tv_sec+r.ru_stime.tv_sec)*1000000000+
           ((uint64_t)r.ru_utime.tv_usec+r.ru_stime.tv_usec)*1000;
}
static ABI __attribute__((noinline)) uintptr_t trivial(uintptr_t a,uintptr_t b,uintptr_t c,uintptr_t d) {
    __asm__ volatile("" : "+r"(a)); return a+b+c+d;
}
static Call resolve(const char *nid) {
    uintptr_t p=runtime_resolve(nid,0);
    if(!p){fprintf(stderr,"Unresolved benchmark import: %s\n",nid);abort();}
    return (Call)p;
}
static Call plt(Call target) {
    size_t size=(size_t)getpagesize();
    unsigned char *p=mmap((void *)UINT64_C(0x900000000),size,PROT_READ|PROT_WRITE,
                         MAP_PRIVATE|MAP_ANON,-1,0);
    assert(p!=MAP_FAILED && (uintptr_t)p<UINT64_C(0x10000000000));
    /* Resolved guest PLT form: jmp qword [rip+2]; padding; GOT target. */
    const unsigned char op[]={0xff,0x25,2,0,0,0,0x90,0x90};
    memcpy(p,op,sizeof(op)); memcpy(p+8,&target,sizeof(target));
    assert(!mprotect(p,size,PROT_READ|PROT_EXEC)); return (Call)p;
}
enum Kind { TRIVIAL, CLOCK, COUNTER, SEMA_PAIR, STATUS, SELF, SPECIFIC, ERROR_SLOT, TLS, MUTEX_PAIR };
struct Row { const char *name; Call a,b; uintptr_t x,y; enum Kind kind; };
static void validate(const struct Row *row) {
    Call a=row->a,b=row->b; uintptr_t x=row->x,y=row->y;
    switch(row->kind) {
    case TRIVIAL: assert(a(x,y,0,0)==x+y); break;
    case CLOCK: {
        int64_t *ts=(int64_t *)y; ts[0]=ts[1]=-1;
        assert((int32_t)a(x,y,0,0)==0);
        assert(ts[0]>=0 && ts[1]>=0 && ts[1]<1000000000);
        int64_t sec=ts[0],ns=ts[1];
        assert((int32_t)a(x,y,0,0)==0);
        assert(ts[0]>=0 && ts[1]>=0 && ts[1]<1000000000);
        if(x==4)assert(ts[0]>sec || (ts[0]==sec && ts[1]>=ns));
        break;
    }
    case COUNTER: { uintptr_t first=a(x,y,0,0); assert(a(x,y,0,0)>=first); break; }
    case SEMA_PAIR: {
        uint32_t timeout=0;
        assert((int32_t)a(x,y,(uintptr_t)&timeout,0)==0);
        assert((int32_t)a(x,y,(uintptr_t)&timeout,0)==(int32_t)UINT32_C(0x8002003c));
        assert((int32_t)b(x,y,0,0)==0);
        assert((int32_t)b(x,y,0,0)==(int32_t)UINT32_C(0x80020016));
        assert((int32_t)a(x,y,(uintptr_t)&timeout,0)==0);
        assert((int32_t)b(x,y,0,0)==0);
        break;
    }
    case STATUS: assert((int32_t)a(x,y,0,0)==0); break;
    case SELF: {
        uintptr_t pointer=a(x,y,0,0),word=0; mach_vm_size_t copied=0;
        assert(pointer && pointer==a(x,y,0,0));
        assert(mach_vm_read_overwrite(mach_task_self(),pointer,sizeof(word),
                                     (mach_vm_address_t)&word,&copied)==KERN_SUCCESS);
        assert(copied==sizeof(word)); break;
    }
    case SPECIFIC: assert(a(x,y,0,0)==42); break;
    case ERROR_SLOT: {
        int32_t *slot=runtime_errno(); int32_t saved=*slot;
        *slot=123; assert(a(x,y,0,0)==(uintptr_t)slot && *slot==123); *slot=saved;
        break;
    }
    case TLS: {
        unsigned char *pointer=(unsigned char *)a(x,y,0,0);
        assert(pointer && pointer[0]==42 && pointer[1]==0 && pointer[31]==0);
        assert((uintptr_t)pointer==a(x,y,0,0)); break;
    }
    case MUTEX_PAIR: {
        Call attempt=resolve("upoVrzMHFeE#p#J");
        assert((int32_t)a(x,y,0,0)==0);
        assert((int32_t)attempt(x,y,0,0)==(int32_t)UINT32_C(0x80020010));
        assert((int32_t)b(x,y,0,0)==0);
        assert((int32_t)attempt(x,y,0,0)==0);
        assert((int32_t)b(x,y,0,0)==0); break;
    }
    }
}
static void measure(const char *name,Call a,Call b,uintptr_t x,uintptr_t y,unsigned n,unsigned round) {
    for(unsigned i=0;i<10000;i++){sink=a(x,y,0,0);if(b)sink=b(x,y,0,0);}
    uint64_t w=wall_ns(),c=cpu_ns();
    for(unsigned i=0;i<n;i++){sink=a(x,y,0,0);if(b)sink=b(x,y,0,0);}
    c=cpu_ns()-c;w=wall_ns()-w;
    printf("%s,%u,%u,%u,%.3f,%.3f\n",name,round,n,b?2:1,(double)c/n,(double)w/n);
    fflush(stdout);
}
int main(void) {
    assert(!getenv("BB_HLE_TRACE")&&!getenv("BB_HLE_TRACE_SUMMARY"));
    assert(!mach_timebase_info(&timebase));
    runtime_start(1);
    runtime_thread_attach_main();
    void *mutex=NULL,*cond=NULL;
    assert(!resolve("cmo1RIYva9o#p#J")((uintptr_t)&mutex,0,0,0));
    assert(!resolve("2Tb92quprl0#p#J")((uintptr_t)&cond,0,0,0));
    uint32_t key=0;
    assert(!resolve("geDaqgH9lTg#p#J")((uintptr_t)&key,0,0,0));
    assert(!resolve("+BzXYkqYeLE#p#J")(key,42,0,0));
    static const unsigned char initial[]={42};
    runtime_set_libc_tls(initial,sizeof(initial),32);
    uint64_t tls_index[]={2,0}; int64_t ts[2];
    uint32_t sema=0;
    typedef int32_t (ABI *CreateSema)(uint32_t *,const char *,uint32_t,int32_t,int32_t,const void *);
    assert(!((CreateSema)runtime_resolve("188x57JYp0g#p#J",0))(&sema,"hle-cost",1,1,1,NULL));
    Call simple=plt(trivial); assert(simple(1,2,3,4)==10);
    struct Row rows[]={
        {"trivial_no_plt",trivial,NULL,1,2,TRIVIAL},
        { "trivial_plt_call_loop",simple,NULL,1,2,TRIVIAL},
        {"sceKernelClockGettime_monotonic",plt(resolve("QBi7HCK03hw#p#J")),NULL,4,(uintptr_t)ts,CLOCK},
        {"sceKernelClockGettime_realtime",plt(resolve("QBi7HCK03hw#p#J")),NULL,0,(uintptr_t)ts,CLOCK},
        {"sceKernelGetProcessTime",plt(resolve("4J2sUJmuHZQ#p#J")),NULL,0,0,COUNTER},
        {"sceKernelReadTsc",plt(resolve("-2IRUCO--PM#p#J")),NULL,0,0,COUNTER},
        {"sceKernelWaitSema+SignalSema_uncontended",plt(resolve("Zxa0VhQVTsk#p#J")),plt(resolve("4czppHBiriw#p#J")),sema,1,SEMA_PAIR},
        {"sceHttpWaitRequest_zero_timeout_no_events",plt(resolve("qISjDHrxONc#K#L")),NULL,0,0,STATUS},
        {"scePthreadSelf",plt(resolve("aI+OeCz8xrQ#p#J")),NULL,0,0,SELF},
        {"scePthreadGetspecific",plt(resolve("eoht7mQOCmo#p#J")),NULL,key,0,SPECIFIC},
        {"__error",plt(resolve("9BcDykPmo1I#p#J")),NULL,0,0,ERROR_SLOT},
        {"__tls_get_addr",plt(resolve("vNe1w4diLCs#p#J")),NULL,(uintptr_t)tls_index,0,TLS},
        {"scePthreadMutexLock+Unlock_uncontended",plt(resolve("9UK1vLZQft4#p#J")),plt(resolve("tn3VlD0hG60#p#J")),(uintptr_t)&mutex,0,MUTEX_PAIR},
        {"scePthreadCondSignal_no_waiters",plt(resolve("kDh-NfxgMtE#p#J")),NULL,(uintptr_t)&cond,0,STATUS},
        {"scePthreadCondBroadcast_no_waiters",plt(resolve("JGgj7Uvrl+A#p#J")),NULL,(uintptr_t)&cond,0,STATUS}
    };
    puts("case,round,iterations,calls_per_iteration,cpu_ns_per_iteration,wall_ns_per_iteration");
    for(unsigned r=0;r<5;r++)for(unsigned j=0;j<sizeof(rows)/sizeof(rows[0]);j++){
        unsigned i=r%2?sizeof(rows)/sizeof(rows[0])-1-j:j;
        validate(&rows[i]);
        measure(rows[i].name,rows[i].a,rows[i].b,rows[i].x,rows[i].y,1000000,r);
        validate(&rows[i]);
    }
    assert(!resolve("R1Jvn8bSCW8#p#J")(sema,0,0,0));
    assert(!resolve("2Of0f+3mhhE#p#J")((uintptr_t)&mutex,0,0,0));
    assert(!resolve("g+PZd2hiacg#p#J")((uintptr_t)&cond,0,0,0));
    fputs("Validated all 75 measurements before and after timed loops\n",stderr);
    return 0;
}
