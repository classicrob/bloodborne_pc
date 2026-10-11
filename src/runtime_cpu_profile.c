#include "runtime_cpu_profile.h"
#include "guest_backtrace.h"
#if defined(__APPLE__) && defined(__x86_64__)
#include <dlfcn.h>
#include <errno.h>
#include <inttypes.h>
#include <mach/mach_time.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/ucontext.h>
#include <time.h>
#include <unistd.h>

#define PROFILE_THREADS 256
#define PROFILE_RING 512
#define PROFILE_DEPTH 12
#define PROFILE_IMAGES 512
_Static_assert(ATOMIC_LLONG_LOCK_FREE==2 && ATOMIC_POINTER_LOCK_FREE==2 && ATOMIC_INT_LOCK_FREE==2,
               "CPU profiler atomics must be lock free");
typedef struct { uint64_t ticks; uintptr_t pc[PROFILE_DEPTH]; unsigned depth,flags; char name[64]; } Sample;
typedef struct {
    _Atomic uintptr_t key;
    _Atomic unsigned long long write,read,dropped;
    _Atomic int unwind;
    uintptr_t low[2],high[2];
    char name[64];
    Sample samples[PROFILE_RING];
} ProfileThread;
typedef struct { uintptr_t low,high; uint64_t ticks; char name[1024],uuid[33]; int removed; } ProfileImage;
static ProfileThread *threads;
static ProfileImage images[PROFILE_IMAGES];
static _Atomic unsigned image_count;
static _Atomic int configured,active,stop_requested,joined;
static _Atomic unsigned sampling_handlers;
static _Atomic unsigned long long table_full,image_full,read_faults;
static pthread_t collector;
static FILE *output;
static pthread_mutex_t output_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t image_lock=PTHREAD_MUTEX_INITIALIZER;
static uint64_t started,start_ticks,end_ticks;
static unsigned frequency=1000;
static mach_timebase_info_data_t timebase;
static struct sigaction previous_stop;
static pthread_key_t profile_key;
extern void runtime_thread_set_profile_hook(void (*)(const char *,void *,size_t));

static void profile_fail(const char *why) { fprintf(stderr,"STOP: BB_CPU_PROFILE %s: %s\n",why,strerror(errno));abort(); }
static ProfileThread *thread_slot(int create) {
    uintptr_t key=(uintptr_t)pthread_self();
    unsigned begin=(unsigned)((key>>12)%PROFILE_THREADS);
    for(unsigned i=0;i<PROFILE_THREADS;i++) {
        ProfileThread *t=&threads[(begin+i)%PROFILE_THREADS];
        uintptr_t seen=atomic_load_explicit(&t->key,memory_order_acquire);
        if(seen==key)return t;
        if(!seen && create && atomic_compare_exchange_strong_explicit(&t->key,&seen,key,memory_order_acq_rel,memory_order_acquire))return t;
    }
    if(create)atomic_fetch_add_explicit(&table_full,1,memory_order_relaxed);
    return NULL;
}
/* Both faulting load PCs are owned by this profiler. No partially read record is
 * consumed on failure; the exception hook redirects to a zero-return label. */
extern int cpu_profile_read(uintptr_t,uintptr_t *);
extern char cpu_profile_load0[],cpu_profile_load1[],cpu_profile_read_failed[];
__asm__(".text\n.globl _cpu_profile_read\n_cpu_profile_read:\n"
        ".globl _cpu_profile_load0\n_cpu_profile_load0: mov (%rdi),%rax\n"
        ".globl _cpu_profile_load1\n_cpu_profile_load1: mov 8(%rdi),%rdx\n"
        "mov %rax,(%rsi)\nmov %rdx,8(%rsi)\nmov $1,%eax\nret\n"
        ".globl _cpu_profile_read_failed\n_cpu_profile_read_failed: xor %eax,%eax\nret\n");
int runtime_cpu_profile_fault(int sig,void *context) {
    if((sig!=SIGBUS && sig!=SIGSEGV) || !atomic_load_explicit(&configured,memory_order_relaxed))return 0;
    ucontext_t *uc=context;uintptr_t pc=uc->uc_mcontext->__ss.__rip;
    if(pc!=(uintptr_t)cpu_profile_load0 && pc!=(uintptr_t)cpu_profile_load1)return 0;
    ProfileThread *t=thread_slot(0);
    if(!t || !atomic_load_explicit(&t->unwind,memory_order_relaxed))return 0;
    uc->uc_mcontext->__ss.__rip=(uintptr_t)cpu_profile_read_failed;
    atomic_fetch_add_explicit(&read_faults,1,memory_order_relaxed);return 1;
}
static void profile_sample(int sig,siginfo_t *info,void *context) {
    (void)sig;(void)info;
    int saved=errno;
    atomic_fetch_add_explicit(&sampling_handlers,1,memory_order_seq_cst);
    if(!atomic_load_explicit(&active,memory_order_seq_cst)){atomic_fetch_sub_explicit(&sampling_handlers,1,memory_order_release);errno=saved;return;}
    uint64_t ticks=mach_absolute_time();
    if(ticks-started<start_ticks || (end_ticks && ticks-started>=end_ticks)){
        atomic_fetch_sub_explicit(&sampling_handlers,1,memory_order_release);errno=saved;return;
    }
    ProfileThread *t=thread_slot(1);
    if(!t){atomic_fetch_sub_explicit(&sampling_handlers,1,memory_order_release);errno=saved;return;}
    uint64_t w=atomic_load_explicit(&t->write,memory_order_relaxed);
    if(w-atomic_load_explicit(&t->read,memory_order_acquire)>=PROFILE_RING){
        atomic_fetch_add_explicit(&t->dropped,1,memory_order_relaxed);atomic_fetch_sub_explicit(&sampling_handlers,1,memory_order_release);errno=saved;return;
    }
    Sample *s=&t->samples[w%PROFILE_RING];
    const x86_thread_state64_t *ss=&((ucontext_t *)context)->uc_mcontext->__ss;
    s->ticks=ticks;s->depth=1;s->flags=0;s->pc[0]=ss->__rip;
    for(unsigned i=0;i<sizeof(s->name);i++)s->name[i]=t->name[i];
    if(!t->high[0])s->flags|=1; /* Unknown thread: RIP only. */
    atomic_store_explicit(&t->unwind,1,memory_order_relaxed);
    int failed=0;
    s->depth+=(unsigned)guest_backtrace_bounded(ss->__rbp,s->pc+1,PROFILE_DEPTH-1,
                                               t->low,t->high,cpu_profile_read,&failed);
    if(failed)s->flags|=2;
    atomic_store_explicit(&t->unwind,0,memory_order_relaxed);
    atomic_store_explicit(&t->write,w+1,memory_order_release);
    atomic_fetch_sub_explicit(&sampling_handlers,1,memory_order_release);
    errno=saved;
}
static void profile_request_stop(int sig) { (void)sig;atomic_store_explicit(&stop_requested,1,memory_order_relaxed); }
static void block_profile(sigset_t *old) {
    sigset_t set;sigemptyset(&set);sigaddset(&set,SIGPROF);
    int e=pthread_sigmask(SIG_BLOCK,&set,old);if(e){errno=e;profile_fail("block signal");}
}
static void restore_mask(const sigset_t *old) {int e=pthread_sigmask(SIG_SETMASK,old,NULL);if(e){errno=e;profile_fail("restore mask");}}
static void profile_thread_exit(void *value) {
    ProfileThread *t=value;
    sigset_t old;block_profile(&old);
    /* A pthread address can be reused. Samples already contain their own name;
     * a later unregistered thread at this address must be RIP-only again. */
    memset(t->low,0,sizeof(t->low));memset(t->high,0,sizeof(t->high));
    memset(t->name,0,sizeof(t->name));
    restore_mask(&old);
}
void runtime_cpu_profile_thread(const char *name) {
    if(!atomic_load_explicit(&configured,memory_order_relaxed))return;
    int saved=errno;sigset_t old;block_profile(&old);
    ProfileThread *t=thread_slot(1);
    if(t) {
        int e=pthread_setspecific(profile_key,t);if(e){errno=e;profile_fail("cache thread lifetime");}
        t->high[0]=(uintptr_t)pthread_get_stackaddr_np(pthread_self());
        size_t size=pthread_get_stacksize_np(pthread_self());
        t->low[0]=size<=t->high[0] ? t->high[0]-size : 0;
        memset(t->name,0,sizeof(t->name));
        if(name)snprintf(t->name,sizeof(t->name),"%s",name);
        else if(pthread_getname_np(pthread_self(),t->name,sizeof(t->name)))t->name[0]=0;
    }
    restore_mask(&old);errno=saved;
}
void runtime_cpu_profile_stack(void *base,size_t size) {
    if(!atomic_load_explicit(&configured,memory_order_relaxed))return;
    int saved=errno;sigset_t old;block_profile(&old);
    ProfileThread *t=thread_slot(1);
    if(t){t->low[1]=(uintptr_t)base;t->high[1]=size<=UINTPTR_MAX-(uintptr_t)base ? (uintptr_t)base+size : 0;}
    restore_mask(&old);errno=saved;
}
static void thread_hook(const char *name,void *base,size_t size) {
    if(name)runtime_cpu_profile_thread(name);else runtime_cpu_profile_stack(base,size);
}
static void json_string(FILE *f,const char *s) {
    fputc('"',f);for(;*s;s++){unsigned char c=(unsigned char)*s;if(c=='"'||c=='\\')fprintf(f,"\\%c",c);else if(c<32)fprintf(f,"\\u%04x",c);else fputc(c,f);}fputc('"',f);
}
void runtime_cpu_profile_guest(uintptr_t base,size_t size,int module) {
    if(!atomic_load_explicit(&configured,memory_order_relaxed))return;
    pthread_mutex_lock(&output_lock);
    fprintf(output,"{\"type\":\"guest\",\"base\":%llu,\"size\":%llu,\"module\":%d}\n",(unsigned long long)base,(unsigned long long)size,module);
    fflush(output);pthread_mutex_unlock(&output_lock);
}
static void publish_image(const ProfileImage *image) {
    pthread_mutex_lock(&image_lock);
    unsigned n=atomic_load_explicit(&image_count,memory_order_relaxed);
    if(n>=PROFILE_IMAGES)atomic_fetch_add(&image_full,1);
    else {images[n]=*image;atomic_store_explicit(&image_count,n+1,memory_order_release);}
    pthread_mutex_unlock(&image_lock);
}
static void image_added(const struct mach_header *header,intptr_t slide) {
    ProfileImage value={0},*out=&value;
    out->ticks=mach_absolute_time();
    if(header->magic!=MH_MAGIC_64)return;
    Dl_info info;if(dladdr(header,&info)&&info.dli_fname)snprintf(out->name,sizeof(out->name),"%s",info.dli_fname);
    const struct load_command *lc=(const void *)((const struct mach_header_64 *)header+1);
    for(uint32_t i=0;i<header->ncmds;i++) {
        if(lc->cmd==LC_SEGMENT_64){const struct segment_command_64 *s=(const void *)lc;if(!strcmp(s->segname,"__TEXT")){out->low=s->vmaddr+slide;out->high=out->low+s->vmsize;}}
        if(lc->cmd==LC_UUID){const unsigned char *u=((const struct uuid_command *)lc)->uuid;for(unsigned j=0;j<16;j++)snprintf(out->uuid+2*j,3,"%02x",u[j]);}
        lc=(const void *)((const char *)lc+lc->cmdsize);
    }
    publish_image(out);
}
static void image_removed(const struct mach_header *header,intptr_t slide) {
    (void)slide;
    ProfileImage value={0};
    value.ticks=mach_absolute_time();value.low=(uintptr_t)header;value.removed=1;
    publish_image(&value);
}
static void set_timer(int on) {
    struct itimerval timer={0};
    if(on){unsigned us=1000000/frequency;timer.it_interval.tv_sec=us/1000000;timer.it_interval.tv_usec=us%1000000;timer.it_value=timer.it_interval;}
    if(setitimer(ITIMER_PROF,&timer,NULL))profile_fail("setitimer");
}
static void *profile_collect(void *arg) {
    (void)arg;sigset_t old;block_profile(&old);pthread_setname_np("BB CPU profile");
    unsigned image_read=0;uint64_t written=0;
    int armed=0;
    for(;;) {
        uint64_t now=mach_absolute_time();
        int stop=atomic_load_explicit(&stop_requested,memory_order_relaxed) || (end_ticks && now-started>=end_ticks);
        if(!armed && !stop && now-started>=start_ticks){atomic_store_explicit(&active,1,memory_order_relaxed);set_timer(1);armed=1;}
        if(stop){set_timer(0);atomic_store_explicit(&active,0,memory_order_seq_cst);
            while(atomic_load_explicit(&sampling_handlers,memory_order_seq_cst)) { struct timespec pause={0,100000};nanosleep(&pause,NULL); }}
        pthread_mutex_lock(&output_lock);
        unsigned count=atomic_load_explicit(&image_count,memory_order_acquire);
        for(;image_read<count;image_read++) {
            ProfileImage *im=&images[image_read];
            if(im->removed){fprintf(output,"{\"type\":\"image-remove\",\"base\":%llu,\"ticks\":%llu}\n",(unsigned long long)im->low,(unsigned long long)im->ticks);continue;}
            fprintf(output,"{\"type\":\"image\",\"base\":%llu,\"end\":%llu,\"ticks\":%llu,\"uuid\":\"%s\",\"path\":",(unsigned long long)im->low,(unsigned long long)im->high,(unsigned long long)im->ticks,im->uuid);
            json_string(output,im->name);fputs("}\n",output);
        }
        for(unsigned i=0;i<PROFILE_THREADS;i++) {
            ProfileThread *t=&threads[i];uintptr_t key=atomic_load_explicit(&t->key,memory_order_acquire);if(!key)continue;
            uint64_t r=atomic_load_explicit(&t->read,memory_order_relaxed),w=atomic_load_explicit(&t->write,memory_order_acquire);
            for(;r<w;r++) {
                const Sample *s=&t->samples[r%PROFILE_RING];
                fprintf(output,"{\"type\":\"sample\",\"pthread\":%llu,\"ticks\":%llu,\"flags\":%u,\"name\":",(unsigned long long)key,(unsigned long long)s->ticks,s->flags);
                json_string(output,s->name);fputs(",\"pc\":[",output);
                for(unsigned j=0;j<s->depth;j++)fprintf(output,"%s%llu",j?",":"",(unsigned long long)s->pc[j]);
                fputs("]}\n",output);written++;
            }
            atomic_store_explicit(&t->read,r,memory_order_release);
        }
        if(fflush(output)||ferror(output))profile_fail("write output");
        pthread_mutex_unlock(&output_lock);
        if(stop)break;
        struct timespec sleep={0,20000000};while(nanosleep(&sleep,&sleep) && errno==EINTR){}
    }
    pthread_mutex_lock(&output_lock);
    uint64_t dropped=0;for(unsigned i=0;i<PROFILE_THREADS;i++)dropped+=atomic_load(&threads[i].dropped);
    fprintf(output,"{\"type\":\"summary\",\"samples\":%llu,\"ring_dropped\":%llu,\"thread_table_full\":%llu,\"image_table_full\":%llu,\"read_faults\":%llu}\n",(unsigned long long)written,(unsigned long long)dropped,(unsigned long long)atomic_load(&table_full),(unsigned long long)atomic_load(&image_full),(unsigned long long)atomic_load(&read_faults));
    if(fflush(output))profile_fail("flush output");pthread_mutex_unlock(&output_lock);
    return NULL;
}
void runtime_cpu_profile_stop(void) {
    if(!atomic_load_explicit(&configured,memory_order_relaxed))return;
    if(atomic_exchange(&joined,1))return;
    atomic_store(&stop_requested,1);int e=pthread_join(collector,NULL);if(e){errno=e;profile_fail("join collector");}
    /* Keep the inactive SIGPROF handler for a pending timer signal. */
}
void runtime_cpu_profile_init(void) {
    const char *path=getenv("BB_CPU_PROFILE");if(!path||!*path||!strcmp(path,"0"))return;
    if(atomic_load(&configured))return;
    const char *hz=getenv("BB_CPU_PROFILE_HZ");if(hz){char *end;unsigned long n=strtoul(hz,&end,10);if(!*hz||*end||n<1||n>4000){errno=EINVAL;profile_fail("frequency must be 1..4000 Hz");}frequency=(unsigned)n;}
    double start=0,end=0;const char *window=getenv("BB_CPU_PROFILE_WINDOW");
    if(window){char tail;if(sscanf(window,"%lf:%lf%c",&start,&end,&tail)!=2||!isfinite(start)||!isfinite(end)||start<0||end<=start||end>86400){errno=EINVAL;profile_fail("window must be start:end seconds, 0 <= start < end <= 86400");}}
    if(mach_timebase_info(&timebase))profile_fail("timebase");
    start_ticks=(uint64_t)(start*1e9*timebase.denom/timebase.numer);end_ticks=(uint64_t)(end*1e9*timebase.denom/timebase.numer);
    struct itimerval old;if(getitimer(ITIMER_PROF,&old)||old.it_value.tv_sec||old.it_value.tv_usec){errno=EBUSY;profile_fail("ITIMER_PROF already used");}
    struct sigaction before;if(sigaction(SIGPROF,NULL,&before)||before.sa_handler!=SIG_DFL){errno=EBUSY;profile_fail("SIGPROF already used");}
    if(sigaction(SIGUSR1,NULL,&previous_stop)||previous_stop.sa_handler!=SIG_DFL){errno=EBUSY;profile_fail("SIGUSR1 already used");}
    threads=calloc(PROFILE_THREADS,sizeof(*threads));if(!threads)profile_fail("rings allocation");
    output=fopen(!strcmp(path,"1")?"cpu-profile.jsonl":path,"wx");if(!output)profile_fail("create output (must not exist)");
    started=mach_absolute_time();
    fprintf(output,"{\"type\":\"config\",\"version\":1,\"hz\":%u,\"numer\":%u,\"denom\":%u,\"start_ticks\":%llu,\"window_start\":%.6f,\"window_end\":%.6f,\"pid\":%d}\n",frequency,timebase.numer,timebase.denom,(unsigned long long)started,start,end,getpid());fflush(output);
    _dyld_register_func_for_add_image(image_added);
    _dyld_register_func_for_remove_image(image_removed);
    struct sigaction sa={0};sa.sa_sigaction=profile_sample;sa.sa_flags=SA_SIGINFO|SA_RESTART;sigemptyset(&sa.sa_mask);
    if(sigaction(SIGPROF,&sa,NULL))profile_fail("SIGPROF handler");
    sa=(struct sigaction){0};sa.sa_handler=profile_request_stop;sa.sa_flags=SA_RESTART;sigemptyset(&sa.sa_mask);
    if(sigaction(SIGUSR1,&sa,NULL))profile_fail("SIGUSR1 handler");
    int key_error=pthread_key_create(&profile_key,profile_thread_exit);if(key_error){errno=key_error;profile_fail("thread lifetime key");}
    atomic_store(&configured,1);runtime_cpu_profile_thread(NULL);
    runtime_thread_set_profile_hook(thread_hook);
    int e=pthread_create(&collector,NULL,profile_collect,NULL);if(e){errno=e;profile_fail("collector thread");}
    if(atexit(runtime_cpu_profile_stop))profile_fail("exit hook");
    fprintf(stderr,"BB_CPU_PROFILE: requested %u Hz process CPU timer; window %.3f:%.3f s; SIGUSR1 stops/dumps; output %s\n",frequency,start,end,path);
}
#endif
