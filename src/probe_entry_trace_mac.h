#ifndef BB_PROBE_ENTRY_TRACE_MAC_H
#define BB_PROBE_ENTRY_TRACE_MAC_H
#include <stdatomic.h>
#include <mach/mach_time.h>
#include <mach/thread_info.h>
#include <pthread.h>

/* Startup-only installation. A retained INT3 redirects to an immutable trampoline
 * which executes the displaced push rbp and jumps back without register scratch.
 * No unpatch/single-step window exists for concurrent function entries. */
#define ENTRY_TRACE_LIMIT 16
typedef struct { uintptr_t address, trampoline; unsigned flags; } EntryTraceSite;
static EntryTraceSite entry_trace_sites[ENTRY_TRACE_LIMIT];
static volatile sig_atomic_t entry_trace_count;
static uint32_t entry_trace_tcb_offset;
static mach_port_t entry_trace_task;
static _Atomic unsigned long long entry_trace_sequence;
_Static_assert(ATOMIC_LLONG_LOCK_FREE==2,"entry trace signal counter must be lock-free");
static _Atomic int entry_trace_active=1;
_Static_assert(ATOMIC_INT_LOCK_FREE==2,"entry trace arm flag must be lock-free");
static int entry_trace_delayed;
static int32_t (__attribute__((sysv_abi)) *entry_trace_start_target)(void *);
static const char *entry_trace_import_error;

/* Setup can fail after renderer threads have started. Do not run process/UI
 * teardown against that partially initialized state on a diagnostic refusal. */
static void entry_trace_require(const char *error) {
    if (!error) return;
    fprintf(stderr,"ERROR: %s\n",error);
    fflush(stderr);
    _Exit(23);
}

static const char *entry_trace_configure(const char *sites,const char *arm) {
    if (!arm || !*arm) return NULL;
    if (!sites || !*sites || !strcmp(sites,"0")) return "BB_GUEST_ENTRY_ARM requires BB_GUEST_ENTRY_TRACE";
    if (strcmp(arm,"avplayer-start")) return "BB_GUEST_ENTRY_ARM must be avplayer-start";
    entry_trace_delayed=1;
    atomic_store_explicit(&entry_trace_active,0,memory_order_relaxed);
    return NULL;
}
static __attribute__((sysv_abi,noinline)) int32_t entry_trace_avplayer_start(void *handle) {
    int saved_errno=errno;
    if (!atomic_exchange_explicit(&entry_trace_active,1,memory_order_relaxed)) {
        static const char line[]="GUEST-ENTRY armed before sceAvPlayerStart\n";
        (void)write(2,line,sizeof(line)-1);
    }
    errno=saved_errno;
    return entry_trace_start_target(handle);
}
/* Relocation-time only, before any guest thread starts. The resolved target may
 * itself be the HLE trace wrapper; retain it unchanged behind this typed hook. */
static uintptr_t entry_trace_wrap_import(const char *name,uintptr_t target) {
    if (!entry_trace_delayed || !target || !name || strcmp(name,"sceAvPlayerStart")) return target;
    if (entry_trace_start_target && (uintptr_t)entry_trace_start_target!=target)
        entry_trace_import_error="BB_GUEST_ENTRY_ARM found inconsistent sceAvPlayerStart targets";
    entry_trace_start_target=(int32_t (__attribute__((sysv_abi)) *)(void *))target;
    return (uintptr_t)entry_trace_avplayer_start;
}

static char *entry_text(char *out,const char *s) { while (*s) *out++=*s++; return out; }
static char *entry_hex(char *out,uint64_t n) {
    static const char digits[]="0123456789abcdef";
    for (unsigned i=0;i<16;++i) *out++=digits[(n>>(60-4*i))&15];
    return out;
}
static char *entry_field(char *out,const char *name,uint64_t n) {
    out=entry_text(out,name); out=entry_text(out,"=0x"); out=entry_hex(out,n); *out++=' '; return out;
}
static kern_return_t entry_read(uintptr_t at,void *out,size_t size) {
    if (size && at>UINTPTR_MAX-(size-1)) return KERN_INVALID_ADDRESS;
    mach_vm_size_t got=0;
    kern_return_t error=mach_vm_read_overwrite(entry_trace_task,at,size,(mach_vm_address_t)out,&got);
    return error==KERN_SUCCESS && got!=size ? KERN_INVALID_ADDRESS : error;
}
static char *entry_words(char *out,const char *name,uintptr_t address) {
    uint64_t words[8];
    out=entry_text(out,name); *out++='[';
    kern_return_t error=entry_read(address,words,sizeof(words));
    if (error) out=entry_field(out,"unreadable-mach",(unsigned)error);
    else for (unsigned i=0;i<8;++i) { out=entry_hex(out,words[i]); *out++=','; }
    *out++=']'; *out++=' '; return out;
}
static char *entry_host_thread(char *out) {
    out=entry_field(out,"pthread",(uintptr_t)pthread_self());
    thread_extended_info_data_t info;
    mach_msg_type_number_t count=THREAD_EXTENDED_INFO_COUNT;
    mach_port_t port=mach_thread_self();
    kern_return_t error=KERN_INVALID_NAME,release=KERN_SUCCESS;
    if (MACH_PORT_VALID(port)) {
        error=thread_info(port,THREAD_EXTENDED_INFO,(thread_info_t)&info,&count);
        release=mach_port_deallocate(entry_trace_task,port);
    }
    if (error || count!=THREAD_EXTENDED_INFO_COUNT) {
        out=entry_field(out,"name-unreadable-mach",error ? (unsigned)error : KERN_FAILURE);
    } else {
        out=entry_text(out,"name=\"");
        for (size_t i=0;i<sizeof(info.pth_name) && info.pth_name[i];++i) {
            unsigned char c=(unsigned char)info.pth_name[i];
            if (c=='\\' || c=='\"') *out++='\\';
            if (c>=32 && c<127) *out++=(char)c;
            else {
                static const char hex[]="0123456789abcdef";
                *out++='\\'; *out++='x'; *out++=hex[c>>4]; *out++=hex[c&15];
            }
        }
        out=entry_text(out,"\" ");
    }
    if (release) out=entry_field(out,"thread-port-release-mach",(unsigned)release);
    return out;
}
static int entry_trace_handle(int sig,const siginfo_t *info,void *context) {
    if (sig!=SIGTRAP || !entry_trace_count) return 0;
    ucontext_t *uc=context;
    x86_thread_state64_t *s=&uc->uc_mcontext->__ss;
    if (!info || info->si_code!=TRAP_BRKPT || uc->uc_mcontext->__es.__trapno!=3) return 0;
    unsigned i;
    for (i=0;i<(unsigned)entry_trace_count;++i)
        if (s->__rip==entry_trace_sites[i].address+1) break;
    if (i==(unsigned)entry_trace_count) return 0;
    if (!atomic_load_explicit(&entry_trace_active,memory_order_relaxed)) {
        s->__rip=entry_trace_sites[i].trampoline;
        return 1;
    }
    int saved_errno=errno;
    uint64_t seq=atomic_fetch_add_explicit(&entry_trace_sequence,1,memory_order_relaxed)+1;
    uintptr_t tcb;
    /* Same cached, validated GS TSD displacement as the loader's TLS rewrite.
     * Only this guaranteed-present TSD slot is read directly; its value and the
     * guest stack/object pointers are inspected through Mach reads. */
    __asm__ volatile("movq %%gs:(%1),%0" : "=r"(tcb) : "r"((uintptr_t)entry_trace_tcb_offset));
    uint64_t thread=0, caller=0;
    kern_return_t thread_error=entry_read(tcb<=UINTPTR_MAX-16 ? tcb+16 : 0,&thread,8);
    kern_return_t caller_error=entry_read(s->__rsp,&caller,8);
    char line[3072], *end=entry_text(line,"GUEST-ENTRY ");
    end=entry_field(end,"seq",seq); end=entry_field(end,"ticks",mach_absolute_time());
    end=entry_field(end,"site",entry_trace_sites[i].address);
    end=entry_field(end,"thread",thread);
    end=entry_host_thread(end);
    if (thread_error) end=entry_field(end,"thread-unreadable",(unsigned)thread_error);
    end=entry_field(end,"caller",caller);
    if (caller_error) end=entry_field(end,"caller-unreadable",(unsigned)caller_error);
#define ENTRY_REG(name) end=entry_field(end,#name,s->__##name)
    ENTRY_REG(rax); ENTRY_REG(rbx); ENTRY_REG(rcx); ENTRY_REG(rdx);
    ENTRY_REG(rdi); ENTRY_REG(rsi); ENTRY_REG(r8); ENTRY_REG(r9);
    ENTRY_REG(r10); ENTRY_REG(r11); ENTRY_REG(r12); ENTRY_REG(r13);
    ENTRY_REG(r14); ENTRY_REG(r15); ENTRY_REG(rbp); ENTRY_REG(rsp); ENTRY_REG(rflags);
#undef ENTRY_REG
    end=entry_words(end,"rdi-words",s->__rdi); end=entry_words(end,"rsi-words",s->__rsi);
    end=entry_words(end,"rdx-words",s->__rdx);
    *end++='\n';
    const char *at=line; size_t remaining=(size_t)(end-line);
    while (remaining) {
        ssize_t n=write(2,at,remaining);
        if (n>0) { at+=n; remaining-=(size_t)n; }
        else if (n<0 && errno==EINTR) continue;
        else break;
    }
    s->__rip=entry_trace_sites[i].trampoline;
    errno=saved_errno;
    return 1;
}

/* Returns a startup error; never install while any guest code can execute.
 * Entries must be explicitly selected and begin with the supported 0x55 byte. */
static const char *entry_trace_install(const char *setting,unsigned char *base,
                                     const Segment *segments,uint64_t count) {
    if (!setting || !*setting || !strcmp(setting,"0")) return NULL;
    if (entry_trace_count) return "BB_GUEST_ENTRY_TRACE already installed";
    if (entry_trace_import_error) return entry_trace_import_error;
    if (entry_trace_delayed && !entry_trace_start_target)
        return "BB_GUEST_ENTRY_ARM requires a resolved sceAvPlayerStart import";
    EntryTraceSite sites[ENTRY_TRACE_LIMIT]; unsigned used=0;
    const char *at=setting;
    for (;;) {
        if (used==ENTRY_TRACE_LIMIT || *at=='-' || *at=='+') return "BB_GUEST_ENTRY_TRACE invalid site list";
        char *end; errno=0;
        uint64_t offset=strtoull(at,&end,0);
        if (errno || end==at || (*end && *end!=',') || offset>UINTPTR_MAX-(uintptr_t)base)
            return "BB_GUEST_ENTRY_TRACE invalid image offset";
        unsigned flags=0;
        for (uint64_t j=0;j<count;++j)
            if ((segments[j].flags&1) && offset>=segments[j].address &&
                offset-segments[j].address<segments[j].size) { flags=(unsigned)segments[j].flags; break; }
        if (!flags) return "BB_GUEST_ENTRY_TRACE site is not executable image memory";
        uintptr_t address=(uintptr_t)base+offset;
        for (unsigned j=0;j<used;++j) if (sites[j].address==address) return "BB_GUEST_ENTRY_TRACE duplicate site";
        if (*(const unsigned char *)address!=0x55) return "BB_GUEST_ENTRY_TRACE requires push-rbp (55) function entry";
        sites[used++]=(EntryTraceSite){address,0,flags};
        if (!*end) break;
        at=end+1; if (!*at) return "BB_GUEST_ENTRY_TRACE empty site";
    }
    size_t page=(size_t)sysconf(_SC_PAGESIZE);
    if (!page || (page&(page-1))) return "BB_GUEST_ENTRY_TRACE invalid host page size";
    unsigned char *code=runtime_low_map_named(page,PROT_READ|PROT_WRITE,"entry-trampoline");
    if (!code) return "BB_GUEST_ENTRY_TRACE cannot allocate trampoline";
    for (unsigned i=0;i<used;++i) {
        unsigned char *t=code+i*32;
        const unsigned char prefix[]={0x55,0xff,0x25,0,0,0,0};
        memcpy(t,prefix,sizeof(prefix)); uintptr_t resume=sites[i].address+1;
        memcpy(t+sizeof(prefix),&resume,8); sites[i].trampoline=(uintptr_t)t;
    }
    if (mprotect(code,page,PROT_READ|PROT_EXEC)) return "BB_GUEST_ENTRY_TRACE cannot protect trampoline";
    entry_trace_task=mach_task_self(); entry_trace_tcb_offset=runtime_thread_tcb_offset();
    /* Resolve the identity-query calls outside the signal path before patching. */
    char identity[512]; (void)entry_host_thread(identity);
    memcpy(entry_trace_sites,sites,used*sizeof(sites[0])); entry_trace_count=(sig_atomic_t)used;
    for (unsigned i=0;i<used;++i) {
        void *start=(void *)(sites[i].address&~(uintptr_t)(page-1));
        /* Removing execute before modifying a previously translated page, then
         * restoring it, invalidates Rosetta's translation of the old entry. */
        if (mprotect(start,page,PROT_READ|PROT_WRITE)) return "BB_GUEST_ENTRY_TRACE cannot patch entry page";
        *(unsigned char *)sites[i].address=0xcc;
        int prot=PROT_EXEC|((sites[i].flags&4)?PROT_READ:0)|((sites[i].flags&2)?PROT_WRITE:0);
        if (mprotect(start,page,prot)) return "BB_GUEST_ENTRY_TRACE cannot restore entry page";
        fprintf(stderr,"GUEST-ENTRY install offset=0x%" PRIxPTR " site=%p trampoline=%p\n",
                sites[i].address-(uintptr_t)base,(void *)sites[i].address,(void *)sites[i].trampoline);
    }
    return NULL;
}
#endif
