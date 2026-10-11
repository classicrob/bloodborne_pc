#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef BB_TEST_CPU_PROFILE
#include "../src/runtime_cpu_profile.c"
void runtime_thread_set_profile_hook(void (*hook)(const char *,void *,size_t)) { (void)hook; }
#endif
typedef struct { uint64_t address,size,flags; } Segment;
static pthread_key_t key;
static uint32_t runtime_thread_tcb_offset(void) { return (uint32_t)key*8; }
static void *runtime_low_map_named(size_t size,int prot,const char *owner) {
    (void)owner;
    void *p=mmap(NULL,size,prot,MAP_PRIVATE|MAP_ANON,-1,0);
    return p==MAP_FAILED ? NULL : p;
}
#include "../src/probe_entry_trace_mac.h"
extern unsigned char entry_body[],entry_body_end[],entry_return[];
extern void entry_call(uint64_t *);
void *entry_code;
static _Atomic int provenance_printed;
static void trap(int sig,siginfo_t *info,void *context) {
#ifdef BB_TEST_CPU_PROFILE
    if(runtime_cpu_profile_fault(sig,context))return;
    if(sig==SIGTRAP && atomic_load(&configured))raise(SIGPROF);
#endif
    if (!atomic_exchange(&provenance_printed,1)) {
        char line[160], *end=entry_text(line,"INT3-PROVENANCE ");
        end=entry_field(end,"si_code",(unsigned)info->si_code);
        end=entry_field(end,"trapno",((ucontext_t *)context)->uc_mcontext->__es.__trapno);
        *end++='\n'; (void)write(2,line,(size_t)(end-line));
    }
    if (!entry_trace_handle(sig,info,context)) _exit(128+sig);
}
static void check_call(void) {
    uint64_t result[96]={0};
    errno=EDOM;
    entry_call(result);
    assert(errno==EDOM);
    const uint64_t expected[]={1,2,3,4,5,(uintptr_t)result,8,9,10,11,12,13,14,15};
    assert(!memcmp(result,expected,sizeof(expected)));
    assert(result[14]==result[15] && result[15]==result[20]-16);
    assert(result[17]==16 && result[22]==16 && result[20]==result[21]);
    assert((result[16]&0xcd5)==0x45 && (result[23]&0xcd5)==0x45);
    for (unsigned i=32;i<96;++i) assert(result[i]==UINT64_MAX);
}
static unsigned start_calls;
static __attribute__((sysv_abi)) int32_t fake_avplayer_start(void *handle) {
    assert(handle==&start_calls && errno==EDOM);
    assert(atomic_load(&entry_trace_active));
    if (!start_calls++) check_call();
    errno=ERANGE;
    return -123;
}
static void name_thread(const char *name,uint64_t id) {
    char actual[64];
    assert(!pthread_setname_np(name));
#ifdef BB_TEST_CPU_PROFILE
    runtime_cpu_profile_thread(name);
#endif
    assert(!pthread_getname_np(pthread_self(),actual,sizeof(actual)));
    assert(!strcmp(actual,name));
    printf("IDENTITY thread=%" PRIu64 " pthread=%016" PRIxPTR " name=%s\n",
           id,(uintptr_t)pthread_self(),actual);
}
static void *worker(void *arg) {
    uint64_t tcb[3]={0,0,100+(uintptr_t)arg};
    assert(!pthread_setspecific(key,tcb));
    char name[64];
    snprintf(name,sizeof(name),"entry-worker-%" PRIuPTR,(uintptr_t)arg);
    name_thread((uintptr_t)arg ? name : "",tcb[2]);
    for (unsigned i=0;i<4;++i) check_call();
    snprintf(name,sizeof(name),"entry-renamed-%" PRIuPTR,(uintptr_t)arg);
    name_thread(name,tcb[2]);
    for (unsigned i=0;i<4;++i) check_call();
    return NULL;
}
static void teardown_must_not_run(void) { abort(); }
static void check_refusal(unsigned char *image,Segment *segment) {
    int output[2];
    assert(!pipe(output));
    pid_t child=fork();
    assert(child>=0);
    if (!child) {
        close(output[0]);
        assert(dup2(output[1],STDERR_FILENO)==STDERR_FILENO);
        close(output[1]);
        assert(!atexit(teardown_must_not_run));
        entry_trace_require(entry_trace_install("0x1",image,segment,1));
        abort();
    }
    close(output[1]);
    char message[256];size_t used=0;
    for (;;) {
        ssize_t n=read(output[0],message+used,sizeof(message)-1-used);
        if (n<0 && errno==EINTR) continue;
        assert(n>=0);
        if (!n) break;
        used+=(size_t)n;assert(used<sizeof(message)-1);
    }
    message[used]=0;close(output[0]);
    int status;assert(waitpid(child,&status,0)==child);
    assert(WIFEXITED(status) && WEXITSTATUS(status)==23);
    assert(strstr(message,"ERROR: BB_GUEST_ENTRY_TRACE requires push-rbp (55) function entry"));
    entry_trace_require(NULL);
    puts("PASS: rejected entry prints its reason and exits 23 without teardown");
}
int main(int argc,char **argv) {
    (void)argv;
    int delayed=argc>1;
    alarm(30);
    assert(!pthread_key_create(&key,NULL));
    size_t page=(size_t)sysconf(_SC_PAGESIZE);
    unsigned char *image=mmap(NULL,page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
    assert(image!=MAP_FAILED);
    entry_code=image+32;
    memcpy(entry_code,entry_body,(size_t)(entry_body_end-entry_body));
    assert(!mprotect(image,page,PROT_READ|PROT_EXEC));
    check_call(); /* Prime Rosetta translation before installing. */
    Segment segment={0,page,5};
    check_refusal(image,&segment);
    assert(entry_trace_configure(NULL,"avplayer-start"));
    assert(entry_trace_configure("0x20","invalid"));
    assert(!entry_trace_configure("0x20",delayed ? "avplayer-start" : NULL));
    assert(entry_trace_wrap_import("other",123)==123);
    assert(entry_trace_wrap_import(NULL,123)==123);
    assert(!entry_trace_wrap_import("sceAvPlayerStart",0));
    if (delayed) assert(entry_trace_install("0x20",image,&segment,1) && !entry_trace_count);
    uintptr_t start=entry_trace_wrap_import("sceAvPlayerStart",(uintptr_t)fake_avplayer_start);
    assert(start==(delayed ? (uintptr_t)entry_trace_avplayer_start : (uintptr_t)fake_avplayer_start));
    assert(!entry_trace_install(NULL,image,&segment,1) && !entry_trace_count);
    assert(entry_trace_install("0x1",image,&segment,1) && !entry_trace_count);
    assert(entry_trace_install("0x20,0x20",image,&segment,1) && !entry_trace_count);
    assert(entry_trace_install("0x100000",image,&segment,1) && !entry_trace_count);
    assert(entry_trace_install("0x20,",image,&segment,1) && !entry_trace_count);
    segment.flags=4;
    assert(entry_trace_install("0x20",image,&segment,1) && !entry_trace_count);
    segment.flags=5;
    assert(!entry_trace_install("0x20",image,&segment,1));
    struct __darwin_mcontext64 mc={0};
    ucontext_t uc={0}; uc.uc_mcontext=&mc;
    mc.__ss.__rip=(uintptr_t)entry_code+1; mc.__es.__trapno=3;
    siginfo_t info={0}; info.si_code=SI_USER;
    assert(!entry_trace_handle(SIGTRAP,&info,&uc));
    assert(mc.__ss.__rip==(uintptr_t)entry_code+1);
    info.si_code=TRAP_TRACE;
    assert(!entry_trace_handle(SIGTRAP,&info,&uc));
    info.si_code=TRAP_BRKPT; mc.__es.__trapno=1;
    assert(!entry_trace_handle(SIGTRAP,&info,&uc));
    struct sigaction sa={0}; sa.sa_sigaction=trap; sa.sa_flags=SA_SIGINFO;
    sigemptyset(&sa.sa_mask); assert(!sigaction(SIGTRAP,&sa,NULL));
#ifdef BB_TEST_CPU_PROFILE
    assert(!sigaction(SIGBUS,&sa,NULL));assert(!sigaction(SIGSEGV,&sa,NULL));
    runtime_cpu_profile_init();
    while(!atomic_load(&active))usleep(1000);
#endif
    mach_port_t own_port=mach_thread_self();
    mach_port_urefs_t refs_before,refs_after;
    assert(!mach_port_get_refs(mach_task_self(),own_port,MACH_PORT_RIGHT_SEND,&refs_before));
    name_thread("entry-main",0);
    if (delayed) {
        for (unsigned i=0;i<32;++i) check_call();
        assert(!atomic_load(&entry_trace_sequence));
        errno=EDOM;
        assert(((int32_t (__attribute__((sysv_abi)) *)(void *))start)(&start_calls)==-123);
        assert(errno==ERANGE && start_calls==1);
        errno=EDOM;
        assert(((int32_t (__attribute__((sysv_abi)) *)(void *))start)(&start_calls)==-123);
        assert(errno==ERANGE && start_calls==2);
    } else check_call(); /* Missing guest TCB is reported safely. */
    uint64_t tcb[3]={0,0,42}; assert(!pthread_setspecific(key,tcb));
    name_thread("entry-main-renamed",42);
    check_call();
    assert(!mach_port_get_refs(mach_task_self(),own_port,MACH_PORT_RIGHT_SEND,&refs_after));
    assert(refs_before==refs_after);
    assert(!mach_port_deallocate(mach_task_self(),own_port));
    pthread_t threads[4];
    for (uintptr_t i=0;i<4;++i) assert(!pthread_create(&threads[i],NULL,worker,(void *)i));
    for (unsigned i=0;i<4;++i) assert(!pthread_join(threads[i],NULL));
    assert(atomic_load(&entry_trace_sequence)==34);
    pid_t child=fork(); assert(child>=0);
    if (!child) { raise(SIGTRAP); _exit(1); }
    int status; assert(waitpid(child,&status,0)==child);
    assert(WIFEXITED(status) && WEXITSTATUS(status)==128+SIGTRAP);
    printf("PASS: warm translation, 34 concurrent hits, GPR/flags/all-YMM/stack preservation, invalid sites/reads, unknown trap; caller=%016" PRIxPTR "\n",(uintptr_t)entry_return);
    return 0;
}
