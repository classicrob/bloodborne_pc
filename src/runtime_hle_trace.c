#include "runtime_hle_trace.h"
#if defined(__APPLE__) && defined(__x86_64__)
#include "runtime.h"
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <x86intrin.h>

#define TRACE_CAPACITY 8192
#define TRACE_IMPORTS 1024
#define TRACE_DEPTH 1024
#define TRACE_FILTERS 32
_Static_assert(ATOMIC_INT_LOCK_FREE==2,"trace gate must be signal safe");
typedef struct { uintptr_t target,thunk; char name[128],nid[128]; } TraceImport;
typedef struct {
    uint64_t seq,call,ticks,duration,args[4],rax,rdx,caller,pthread;
    unsigned exit,abandoned;
    const TraceImport *import;
    char name[64];
} TraceRecord;
typedef struct { uintptr_t slot,ret; TraceRecord entry; } TraceCall;
static TraceImport imports[TRACE_IMPORTS];
static unsigned import_count;
static pthread_mutex_t import_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t config_once=PTHREAD_ONCE_INIT;
static char filter_storage[4096],*filters[TRACE_FILTERS];
static unsigned filter_count;
static volatile sig_atomic_t enabled;
static int stream;
static _Atomic int gate;
static uint64_t sequence;
static TraceRecord records[TRACE_CAPACITY];
static _Thread_local TraceCall calls[TRACE_DEPTH];
static _Thread_local unsigned depth;
void runtime_hle_trace_entry_bridge(void);
void runtime_hle_trace_return_bridge(void);

static void die(const char *why) {
    fprintf(stderr,"STOP: BB_HLE_TRACE %s\n",why); abort();
}
static void configure(void) {
    const char *setting=getenv("BB_HLE_TRACE");
    if (!setting || !*setting || !strcmp(setting,"0")) return;
    if (strlen(setting)>=sizeof(filter_storage)) die("filter is too long");
    strcpy(filter_storage,setting);
    char *at=filter_storage;
    do {
        if (filter_count==TRACE_FILTERS || !*at) die("invalid filter list");
        filters[filter_count++]=at;
        char *end=strchr(at,',');
        if (!end) break;
        *end=0; at=end+1;
    } while (1);
    stream=getenv("BB_HLE_TRACE_STREAM") && !strcmp(getenv("BB_HLE_TRACE_STREAM"),"1");
    enabled=1;
}
static int selected(const char *nid,const char *name) {
    for (unsigned i=0;i<filter_count;i++) {
        const char *f=filters[i];
        if (!strcmp(f,nid) || !strcmp(f,name)) return 1;
        if (!strcmp(f,"gnm") && strstr(name,"sceGnm") &&
            (strstr(name,"Submit") || strstr(name,"Wait") || strstr(name,"Flip") ||
             strstr(name,"Label"))) return 1;
        if (!strcmp(f,"videoout") && strstr(name,"sceVideoOut")) return 1;
        if (!strcmp(f,"avplayer") && strstr(name,"sceAvPlayer")) return 1;
        if (!strcmp(f,"sync") && (strstr(name,"Mutex") || strstr(name,"mutex") ||
            strstr(name,"Cond") || strstr(name,"cond") || strstr(name,"Sema") ||
            strstr(name,"EventFlag") || strstr(name,"Equeue") || strstr(name,"Usleep") ||
            strstr(name,"usleep") || strstr(name,"Nanosleep") || strstr(name,"nanosleep") ||
            strstr(name,"Yield") || strstr(name,"yield") || strstr(name,"ClockGettime") || strstr(name,"clock_gettime"))) return 1;
    }
    return 0;
}
static char *text_out(char *p,const char *s) { while (*s) *p++=*s++; return p; }
static char *hex_out(char *p,uint64_t n) {
    const char hex[]="0123456789abcdef";
    for (unsigned i=0;i<16;i++) *p++=hex[(n>>(60-i*4))&15];
    return p;
}
static char *field(char *p,const char *name,uint64_t n) {
    p=text_out(p,name);p=text_out(p,"=0x");p=hex_out(p,n);*p++=' ';return p;
}
static void output(const char *p,size_t n) {
    while (n) { ssize_t k=write(STDERR_FILENO,p,n); if(k>0){p+=k;n-=(size_t)k;} else if(k<0&&errno==EINTR)continue;else break; }
}
static void print_record(const TraceRecord *r) {
    char line[1024],*p=line;
    p=text_out(p,"HLE-TRACE ");p=text_out(p,r->exit ? "EXIT " : "ENTER ");
    p=field(p,"seq",r->seq);p=field(p,"call",r->call);p=field(p,"tsc",r->ticks);
    p=field(p,"pthread",r->pthread);p=text_out(p,"name=\"");
    for(unsigned i=0;i<sizeof(r->name)&&r->name[i];i++) {
        unsigned char c=(unsigned char)r->name[i];*p++=c>=32&&c<127&&c!='"'&&c!='\\' ? c : '?';
    }
    p=text_out(p,"\" import=");p=text_out(p,r->import->name);*p++=' ';
    p=field(p,"caller",r->caller);
    p=field(p,"a0",r->args[0]);p=field(p,"a1",r->args[1]);
    p=field(p,"a2",r->args[2]);p=field(p,"a3",r->args[3]);
    if(r->exit){p=field(p,"rax",r->rax);p=field(p,"rdx",r->rdx);p=field(p,"duration_tsc",r->duration);p=field(p,"abandoned",r->abandoned);}
    *p++='\n'; output(line,(size_t)(p-line));
}
static void publish(TraceRecord *r) {
    while (atomic_exchange_explicit(&gate,1,memory_order_acquire)) __asm__ volatile("pause");
    r->seq=++sequence;if(!r->exit)r->call=r->seq;
    records[(r->seq-1)%TRACE_CAPACITY]=*r;
#ifdef BB_HLE_TRACE_TEST
    extern void hle_trace_publication_hook(void);
    hle_trace_publication_hook();
#endif
    atomic_store_explicit(&gate,0,memory_order_release);
    if(stream)print_record(r);
}
void runtime_hle_trace_dump(void) {
    int saved=errno;
    if(!enabled) return;
    if(atomic_exchange_explicit(&gate,1,memory_order_acquire)) {
        static const char busy[]="HLE-TRACE dump unavailable: publication or dump in progress\n";
        output(busy,sizeof(busy)-1);errno=saved;return;
    }
    static const char begin[]="HLE-TRACE ring begin (bounded, oldest retained first)\n";
    output(begin,sizeof(begin)-1);
    uint64_t first=sequence>TRACE_CAPACITY ? sequence-TRACE_CAPACITY : 0;
    for(uint64_t i=first;i<sequence;i++)print_record(&records[i%TRACE_CAPACITY]);
    static const char end[]="HLE-TRACE ring end\n";output(end,sizeof(end)-1);
    atomic_store_explicit(&gate,0,memory_order_release);errno=saved;
}
/* Saved register order is shared with the two assembly bridges below. */
typedef struct { uint64_t r11,r10,r9,r8,rdi,rsi,rdx,rcx,rax,flags,original_r11; } Saved;
__attribute__((used,noinline)) uintptr_t runtime_hle_trace_enter(Saved *s,uintptr_t *slot) {
    int host_errno=errno;int32_t *guest_errno=runtime_errno();int32_t guest_saved=*guest_errno;
    const TraceImport *imp=(void *)s->r11;
    for(unsigned i=depth;i>0;i--) if(calls[i-1].slot==(uintptr_t)slot){depth=i-1;break;}
    if(depth==TRACE_DEPTH)die("active call capacity exhausted (possibly nonlocal exits)");
    TraceCall *c=&calls[depth++];
    *c=(TraceCall){.slot=(uintptr_t)slot,.ret=*slot,.entry={.import=imp,
        .ticks=__rdtsc(),.args={s->rdi,s->rsi,s->rdx,s->rcx},.caller=*slot,
        .pthread=(uintptr_t)pthread_self()}};
    if(pthread_getname_np(pthread_self(),c->entry.name,sizeof(c->entry.name)))
        strcpy(c->entry.name,"<unavailable>");
    publish(&c->entry);
    *slot=(uintptr_t)runtime_hle_trace_return_bridge;
    *guest_errno=guest_saved;errno=host_errno;return imp->target;
}
__attribute__((used,noinline)) uintptr_t runtime_hle_trace_leave(Saved *s,uintptr_t slot) {
    int host_errno=errno;int32_t *guest_errno=runtime_errno();int32_t guest_saved=*guest_errno;
    unsigned i=depth;while(i && calls[i-1].slot!=slot)--i;
    if(!i)die("return slot has no matching active call");
    TraceCall c=calls[i-1];unsigned skipped=depth-i;depth=i-1;
    TraceRecord r=c.entry;r.exit=1;r.ticks=__rdtsc();r.duration=r.ticks-c.entry.ticks;
    r.rax=s->rax;r.rdx=s->rdx;r.abandoned=skipped;publish(&r);
    *guest_errno=guest_saved;errno=host_errno;return c.ret;
}
uintptr_t runtime_hle_trace_wrap(const char *nid,const char *symbol,uintptr_t target) {
    pthread_once(&config_once,configure);
    if(!enabled || !target || !selected(nid,symbol ? symbol : nid))return target;
    int saved=errno;pthread_mutex_lock(&import_lock);
    for(unsigned i=0;i<import_count;i++) if(imports[i].target==target && !strcmp(imports[i].nid,nid)) {
        uintptr_t result=imports[i].thunk;pthread_mutex_unlock(&import_lock);errno=saved;return result;
    }
    if(import_count==TRACE_IMPORTS)die("import capacity exhausted");
    TraceImport *imp=&imports[import_count++];imp->target=target;
    if(strlen(nid)>=sizeof(imp->nid)||strlen(symbol ? symbol : nid)>=sizeof(imp->name))die("import name too long");
    strcpy(imp->nid,nid);strcpy(imp->name,symbol ? symbol : nid);
    size_t page=(size_t)getpagesize();
    unsigned char *code=runtime_low_map_named(page,PROT_READ|PROT_WRITE,"hle-trace-thunk");
    if(!code)die("cannot allocate import thunk");
    const unsigned char prefix[]={0x41,0x53,0x49,0xbb};
    memcpy(code,prefix,sizeof(prefix));uintptr_t value=(uintptr_t)imp;memcpy(code+4,&value,8);
    const unsigned char jump[]={0xff,0x25,0,0,0,0};memcpy(code+12,jump,6);
    value=(uintptr_t)runtime_hle_trace_entry_bridge;memcpy(code+18,&value,8);
    if(mprotect(code,page,PROT_READ|PROT_EXEC))die("cannot make import thunk executable");
    imp->thunk=(uintptr_t)code;
    pthread_mutex_unlock(&import_lock);errno=saved;return imp->thunk;
}

/* Thunks push original r11, then carry immutable metadata in r11. Entry keeps
 * the caller's stack arguments in place, saves every vector register plus x87,
 * and substitutes only the return slot. Return locates that exact slot rather
 * than blindly popping TLS: callbacks may longjmp across inner traced calls.
 * The indirect jump targets live below the restored stack and use no GPR.
 * These diagnostic bridges do not provide C++ exception unwind metadata. */
__asm__(
    ".text\n"
    ".globl _runtime_hle_trace_entry_bridge\n"
    "_runtime_hle_trace_entry_bridge:\n"
    "pushfq\n"
    "push %rax\n"
    "push %rcx\n"
    "push %rdx\n"
    "push %rsi\n"
    "push %rdi\n"
    "push %r8\n"
    "push %r9\n"
    "push %r10\n"
    "push %r11\n"
    "sub $1040, %rsp\n"
    "fxsave64 (%rsp)\n"
    "vmovdqu %ymm0, 512(%rsp)\n"
    "vmovdqu %ymm1, 544(%rsp)\n"
    "vmovdqu %ymm2, 576(%rsp)\n"
    "vmovdqu %ymm3, 608(%rsp)\n"
    "vmovdqu %ymm4, 640(%rsp)\n"
    "vmovdqu %ymm5, 672(%rsp)\n"
    "vmovdqu %ymm6, 704(%rsp)\n"
    "vmovdqu %ymm7, 736(%rsp)\n"
    "vmovdqu %ymm8, 768(%rsp)\n"
    "vmovdqu %ymm9, 800(%rsp)\n"
    "vmovdqu %ymm10, 832(%rsp)\n"
    "vmovdqu %ymm11, 864(%rsp)\n"
    "vmovdqu %ymm12, 896(%rsp)\n"
    "vmovdqu %ymm13, 928(%rsp)\n"
    "vmovdqu %ymm14, 960(%rsp)\n"
    "vmovdqu %ymm15, 992(%rsp)\n"
    "lea 1040(%rsp), %rdi\n"
    "lea 1128(%rsp), %rsi\n"
    "call _runtime_hle_trace_enter\n"
    "mov %rax, 1024(%rsp)\n"
    "vmovdqu 512(%rsp), %ymm0\n"
    "vmovdqu 544(%rsp), %ymm1\n"
    "vmovdqu 576(%rsp), %ymm2\n"
    "vmovdqu 608(%rsp), %ymm3\n"
    "vmovdqu 640(%rsp), %ymm4\n"
    "vmovdqu 672(%rsp), %ymm5\n"
    "vmovdqu 704(%rsp), %ymm6\n"
    "vmovdqu 736(%rsp), %ymm7\n"
    "vmovdqu 768(%rsp), %ymm8\n"
    "vmovdqu 800(%rsp), %ymm9\n"
    "vmovdqu 832(%rsp), %ymm10\n"
    "vmovdqu 864(%rsp), %ymm11\n"
    "vmovdqu 896(%rsp), %ymm12\n"
    "vmovdqu 928(%rsp), %ymm13\n"
    "vmovdqu 960(%rsp), %ymm14\n"
    "vmovdqu 992(%rsp), %ymm15\n"
    "fxrstor64 (%rsp)\n"
    "add $1040, %rsp\n"
    "pop %r11\n"
    "pop %r10\n"
    "pop %r9\n"
    "pop %r8\n"
    "pop %rdi\n"
    "pop %rsi\n"
    "pop %rdx\n"
    "pop %rcx\n"
    "pop %rax\n"
    "popfq\n"
    "pop %r11\n"
    "jmp *-104(%rsp)\n"
    ".text\n"
    ".globl _runtime_hle_trace_return_bridge\n"
    "_runtime_hle_trace_return_bridge:\n"
    "push %r11\n"
    "pushfq\n"
    "push %rax\n"
    "push %rcx\n"
    "push %rdx\n"
    "push %rsi\n"
    "push %rdi\n"
    "push %r8\n"
    "push %r9\n"
    "push %r10\n"
    "push %r11\n"
    "sub $1048, %rsp\n"
    "fxsave64 (%rsp)\n"
    "vmovdqu %ymm0, 512(%rsp)\n"
    "vmovdqu %ymm1, 544(%rsp)\n"
    "vmovdqu %ymm2, 576(%rsp)\n"
    "vmovdqu %ymm3, 608(%rsp)\n"
    "vmovdqu %ymm4, 640(%rsp)\n"
    "vmovdqu %ymm5, 672(%rsp)\n"
    "vmovdqu %ymm6, 704(%rsp)\n"
    "vmovdqu %ymm7, 736(%rsp)\n"
    "vmovdqu %ymm8, 768(%rsp)\n"
    "vmovdqu %ymm9, 800(%rsp)\n"
    "vmovdqu %ymm10, 832(%rsp)\n"
    "vmovdqu %ymm11, 864(%rsp)\n"
    "vmovdqu %ymm12, 896(%rsp)\n"
    "vmovdqu %ymm13, 928(%rsp)\n"
    "vmovdqu %ymm14, 960(%rsp)\n"
    "vmovdqu %ymm15, 992(%rsp)\n"
    "lea 1048(%rsp), %rdi\n"
    "lea 1128(%rsp), %rsi\n"
    "call _runtime_hle_trace_leave\n"
    "mov %rax, 1024(%rsp)\n"
    "vmovdqu 512(%rsp), %ymm0\n"
    "vmovdqu 544(%rsp), %ymm1\n"
    "vmovdqu 576(%rsp), %ymm2\n"
    "vmovdqu 608(%rsp), %ymm3\n"
    "vmovdqu 640(%rsp), %ymm4\n"
    "vmovdqu 672(%rsp), %ymm5\n"
    "vmovdqu 704(%rsp), %ymm6\n"
    "vmovdqu 736(%rsp), %ymm7\n"
    "vmovdqu 768(%rsp), %ymm8\n"
    "vmovdqu 800(%rsp), %ymm9\n"
    "vmovdqu 832(%rsp), %ymm10\n"
    "vmovdqu 864(%rsp), %ymm11\n"
    "vmovdqu 896(%rsp), %ymm12\n"
    "vmovdqu 928(%rsp), %ymm13\n"
    "vmovdqu 960(%rsp), %ymm14\n"
    "vmovdqu 992(%rsp), %ymm15\n"
    "fxrstor64 (%rsp)\n"
    "add $1048, %rsp\n"
    "pop %r11\n"
    "pop %r10\n"
    "pop %r9\n"
    "pop %r8\n"
    "pop %rdi\n"
    "pop %rsi\n"
    "pop %rdx\n"
    "pop %rcx\n"
    "pop %rax\n"
    "popfq\n"
    "pop %r11\n"
    "jmp *-112(%rsp)\n"
);
#else
uintptr_t runtime_hle_trace_wrap(const char *nid,const char *symbol,uintptr_t target) {
    (void)nid;(void)symbol;return target;
}
void runtime_hle_trace_dump(void) {}
#endif
