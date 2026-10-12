#define _GNU_SOURCE
#include <assert.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include "../src/runtime_cpu_profile.c"
#include "../src/runtime_kernel.c"
#include "../src/runtime_sema.c"
#include "test_alarm.h"
static _Thread_local int32_t fixture_errno;
int32_t *runtime_errno(void) { return &fixture_errno; }
void runtime_thread_set_profile_hook(void (*hook)(const char *,void *,size_t)) { (void)hook; }

static void *tracking_page;
static size_t page_size;
static volatile sig_atomic_t tracking_faults;
static _Atomic int waiting;
static int pipe_fd[2];
static volatile uint64_t result;
static volatile sig_atomic_t nested_signal,nested_seen;
static uintptr_t nested_frame;
static void sampled(int sig,siginfo_t *info,void *context) {
    const ucontext_t *interrupted=context;
    if(nested_signal && (sigismember(&interrupted->uc_sigmask,SIGBUS)==1 ||
                         sigismember(&interrupted->uc_sigmask,SIGSEGV)==1)) {
        ucontext_t copy=*(ucontext_t *)context;
        struct __darwin_mcontext64 mc=*copy.uc_mcontext;copy.uc_mcontext=&mc;
        mc.__ss.__rbp=nested_frame;
        ProfileThread *t=thread_slot(0);assert(t);
        uint64_t w=atomic_load(&t->write);profile_sample(sig,info,&copy);
        assert(atomic_load(&t->write)==w+1);
        Sample *sample=&t->samples[w%PROFILE_RING];
        assert(sample->depth==1 && (sample->flags&4) && !(sample->flags&2));
        nested_seen++;return;
    }
    profile_sample(sig,info,context);
}
static void fault(int sig,siginfo_t *info,void *context) {
    if(runtime_cpu_profile_fault(sig,context))return;
    if(nested_signal==sig){assert(!raise(SIGPROF));return;}
    if(tracking_page && (uintptr_t)info->si_addr>=(uintptr_t)tracking_page &&
       (uintptr_t)info->si_addr<(uintptr_t)tracking_page+page_size) {
        if(mprotect(tracking_page,page_size,PROT_READ|PROT_WRITE))_exit(87);
        tracking_faults++;return;
    }
    _exit(86);
}
__attribute__((noinline)) static uint64_t hot_alpha(uint64_t n) {
    uint64_t x=13;
    for(uint64_t i=0;i<n;i++){x=x*6364136223846793005ULL+1;__asm__ volatile("" : "+r"(x));}
    return x;
}
__attribute__((noinline)) static uint64_t hot_beta(uint64_t n) {
    uint64_t x=17;
    for(uint64_t i=0;i<n;i++){x=(x^(x>>13))*2862933555777941757ULL;__asm__ volatile("" : "+r"(x));}
    return x;
}
static double seconds(void) {struct timespec t;assert(!clock_gettime(CLOCK_MONOTONIC,&t));return t.tv_sec+t.tv_nsec*1e-9;}
static double cpu_seconds(void) {struct timespec t;assert(!clock_gettime(CLOCK_PROCESS_CPUTIME_ID,&t));return t.tv_sec+t.tv_nsec*1e-9;}
static void *hot(void *arg) {
    const char *name=arg ? "hot-beta" : "hot-alpha";
    assert(!pthread_setname_np(name));runtime_cpu_profile_thread(name);
    uint64_t value=arg ? hot_beta(180000000) : hot_alpha(180000000);
    return (void *)(uintptr_t)value;
}
static void *uncached(void *arg) {
    (void)arg;for(unsigned i=0;i<20;i++)assert(!raise(SIGPROF));return NULL;
}
static void *blocked(void *arg) {
    (void)arg;runtime_cpu_profile_thread("blocked-pipe");
    atomic_store(&waiting,1);char c=0;errno=EDOM;
    assert(read(pipe_fd[0],&c,1)==1 && c=='x' && errno==EDOM);
    return NULL;
}
static _Atomic int wait_stage;
static uint32_t test_semaphore;
static void *hle_waits(void *arg) {
    (void)arg;runtime_cpu_profile_thread("hle-waits");
    *runtime_errno()=123;
    double start=seconds();atomic_store(&wait_stage,1);
    assert(kernel_usleep(100000)==0 && seconds()-start>=0.09);
    GuestTimespec req={0,100000000},rem={-1,-1};
    start=seconds();atomic_store(&wait_stage,2);
    assert(kernel_nanosleep(&req,&rem)==0 && seconds()-start>=0.09);
    assert(!rem.sec&&!rem.nsec&&*runtime_errno()==123);
    atomic_store(&wait_stage,3);assert(sem_wait(test_semaphore,1,NULL)==0);
    uint32_t timeout=100000;start=seconds();atomic_store(&wait_stage,4);
    assert((uint32_t)sem_wait(test_semaphore,1,&timeout)==0x8002003c && seconds()-start>=0.09);
    atomic_store(&wait_stage,5);return NULL;
}
static void wait_controls(void) {
    assert(!sem_create(&test_semaphore,"profile",1,0,1,NULL));
    pthread_t thread;assert(!pthread_create(&thread,NULL,hle_waits,NULL));
    while(atomic_load(&wait_stage)<3) {assert(!pthread_kill(thread,SIGPROF));usleep(1000);}
    while(!runtime_sema_waiters(test_semaphore))sched_yield();
    for(unsigned i=0;i<20;i++){assert(!pthread_kill(thread,SIGPROF));usleep(1000);assert(atomic_load(&wait_stage)==3);}
    assert(!sem_signal(test_semaphore,1));
    while(atomic_load(&wait_stage)!=5){assert(!pthread_kill(thread,SIGPROF));usleep(1000);}
    assert(!pthread_join(thread,NULL));assert(!sem_delete(test_semaphore));
}
static void synthetic(uintptr_t frame,uintptr_t low,uintptr_t high,int expect_fail) {
    sigset_t mask;block_profile(&mask);
    ProfileThread *t=thread_slot(0);assert(t);
    uintptr_t saved_low=t->low[1],saved_high=t->high[1];
    t->low[1]=low;t->high[1]=high;
    struct __darwin_mcontext64 mc={0};ucontext_t uc={0};uc.uc_mcontext=&mc;
    mc.__ss.__rip=(uintptr_t)hot_alpha;mc.__ss.__rbp=frame;
    uint64_t w=atomic_load(&t->write);errno=E2BIG;
    profile_sample(SIGPROF,NULL,&uc);assert(errno==E2BIG);
    assert(atomic_load(&t->write)==w+1);
    const Sample *s=&t->samples[w%PROFILE_RING];
    assert(!!(s->flags&2)==expect_fail);
    if(expect_fail)assert(s->depth==1);
    t->low[1]=saved_low;t->high[1]=saved_high;restore_mask(&mask);
}
static _Atomic unsigned stop_entered,stop_completed;
static void *stopping(void *arg) {
    (void)arg;atomic_fetch_add(&stop_entered,1);
    runtime_cpu_profile_stop();atomic_fetch_add(&stop_completed,1);return NULL;
}
static void stop_control(void) {
    assert(!raise(SIGPROF));
    pthread_mutex_lock(&output_lock);
    pthread_t a,b;assert(!pthread_create(&a,NULL,stopping,NULL));assert(!pthread_create(&b,NULL,stopping,NULL));
    while(atomic_load(&stop_entered)!=2)sched_yield();
    usleep(30000);assert(!atomic_load(&stop_completed));
    pthread_mutex_unlock(&output_lock);
    assert(!pthread_join(a,NULL));assert(!pthread_join(b,NULL));
    assert(atomic_load(&stop_completed)==2 && atomic_load(&stop_state)==2);
}
static void safety_controls(void) {
    unsigned char *p=mmap(NULL,page_size*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
    assert(p!=MAP_FAILED);
    uintptr_t *frame=(void *)p;frame[0]=(uintptr_t)(frame+2);frame[1]=(uintptr_t)hot_beta;
    frame[2]=0;frame[3]=(uintptr_t)hot_alpha;
    synthetic((uintptr_t)p,(uintptr_t)p,(uintptr_t)p+page_size*2,0);
    assert(!mprotect(p,page_size,PROT_NONE));
    ProfileThread *current=thread_slot(0);assert(current);
    current->low[1]=(uintptr_t)p;current->high[1]=(uintptr_t)p+page_size*2;
    nested_frame=(uintptr_t)p;
    nested_signal=SIGSEGV;assert(!raise(SIGSEGV));
    nested_signal=SIGBUS;assert(!raise(SIGBUS));
    sigset_t blocked,previous;sigemptyset(&blocked);sigaddset(&blocked,SIGBUS);
    assert(!pthread_sigmask(SIG_BLOCK,&blocked,&previous));assert(!raise(SIGPROF));
    assert(!pthread_sigmask(SIG_SETMASK,&previous,NULL));
    nested_signal=0;assert(nested_seen>=3);
    current->low[1]=current->high[1]=0;
    synthetic((uintptr_t)p,(uintptr_t)p,(uintptr_t)p+page_size*2,1);
    assert(!mprotect(p,page_size,PROT_READ|PROT_WRITE));
    assert(!mprotect(p+page_size,page_size,PROT_NONE));
    synthetic((uintptr_t)p+page_size-8,(uintptr_t)p,(uintptr_t)p+page_size*2,1);
    assert(!munmap(p,page_size*2));
    synthetic((uintptr_t)p,(uintptr_t)p,(uintptr_t)p+page_size*2,1);
    synthetic(UINTPTR_MAX-7,UINTPTR_MAX-16,UINTPTR_MAX,0);
    ProfileThread *t=thread_slot(0);assert(t);
    struct __darwin_mcontext64 mc={0};ucontext_t uc={0};uc.uc_mcontext=&mc;
    mc.__ss.__rip=(uintptr_t)cpu_profile_load0;
    assert(!runtime_cpu_profile_fault(SIGBUS,&uc));
    atomic_store(&t->unwind,1);
    assert(!runtime_cpu_profile_fault(SIGTRAP,&uc));
    mc.__ss.__rip=(uintptr_t)hot_alpha;assert(!runtime_cpu_profile_fault(SIGBUS,&uc));
    atomic_store(&t->unwind,0);
    pid_t child=fork();assert(child>=0);
    if(!child){atomic_store(&t->unwind,1);__asm__ volatile("xor %%rax,%%rax; movq $1,(%%rax)" ::: "rax","memory");_exit(88);}
    int status;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==86);
    tracking_page=mmap(NULL,page_size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
    assert(tracking_page!=MAP_FAILED);
    for(unsigned i=0;i<1000;i++) {
        assert(!mprotect(tracking_page,page_size,PROT_READ));
        *(volatile unsigned *)tracking_page=i;
        if(i%16==0)assert(!raise(SIGPROF));
    }
    assert(tracking_faults==1000);assert(!munmap(tracking_page,page_size));tracking_page=NULL;
}
int main(int argc,char **argv) {
    test_alarm(90);page_size=(size_t)sysconf(_SC_PAGESIZE);
    if(argc>1 && !strcmp(argv[1],"bench-off")) {
        unsetenv("BB_CPU_PROFILE");runtime_cpu_profile_init();
        double start=seconds(),cpu=cpu_seconds();result=hot_beta(250000000);printf("BENCH %.9f %.9f\n",seconds()-start,cpu_seconds()-cpu);return 0;
    }
    if(argc>1 && !strcmp(argv[1],"off")) {
        unsetenv("BB_CPU_PROFILE");runtime_cpu_profile_init();assert(!atomic_load(&configured));
        struct sigaction sa;assert(!sigaction(SIGPROF,NULL,&sa)&&sa.sa_handler==SIG_DFL);
        struct itimerval timer;assert(!getitimer(ITIMER_PROF,&timer)&&!timer.it_value.tv_usec&&!timer.it_value.tv_sec);
        puts("CPU profile default-off PASS");return 0;
    }
    struct sigaction sa={0};sa.sa_sigaction=fault;sa.sa_flags=SA_SIGINFO|SA_RESTART;sigemptyset(&sa.sa_mask);
    assert(!sigaction(SIGBUS,&sa,NULL));assert(!sigaction(SIGSEGV,&sa,NULL));
    runtime_cpu_profile_init();runtime_cpu_profile_thread("fixture-main");
    sa.sa_sigaction=sampled;assert(!sigaction(SIGPROF,&sa,NULL));
    assert(atomic_load(&configured));
    while(!atomic_load(&active)){struct timespec t={0,1000000};nanosleep(&t,NULL);}
    if(argc>1 && !strcmp(argv[1],"bench")) {
        double start=seconds(),cpu=cpu_seconds();result=hot_beta(250000000);printf("BENCH %.9f %.9f\n",seconds()-start,cpu_seconds()-cpu);
    } else if(argc>1 && !strcmp(argv[1],"window")) {
        while(atomic_load(&active))result=hot_alpha(1000000);
    } else if(argc>1 && !strcmp(argv[1],"stop-race")) {
        stop_control();
    } else if(argc>1 && !strcmp(argv[1],"overflow")) {
        pthread_mutex_lock(&output_lock);
        for(unsigned i=0;i<PROFILE_RING+100;i++)assert(!raise(SIGPROF));
        assert(atomic_load(&thread_slot(0)->dropped)>=100);
        pthread_mutex_unlock(&output_lock);
    } else {
        safety_controls();
        wait_controls();
        assert(!pipe(pipe_fd));pthread_t a,b,c;
        assert(!pthread_create(&a,NULL,hot,NULL));assert(!pthread_create(&b,NULL,hot,(void *)1));
        assert(!pthread_create(&c,NULL,blocked,NULL));
        while(!atomic_load(&waiting))sched_yield();
        for(unsigned i=0;i<20;i++){assert(!pthread_kill(c,SIGPROF));usleep(1000);}
        assert(write(pipe_fd[1],"x",1)==1);
        assert(!pthread_join(a,NULL));assert(!pthread_join(b,NULL));assert(!pthread_join(c,NULL));
        for(unsigned i=0;i<PROFILE_THREADS;i++) {
            uintptr_t key=atomic_load(&threads[i].key);
            if(key==(uintptr_t)a||key==(uintptr_t)b||key==(uintptr_t)c)assert(!threads[i].high[0]&&!threads[i].name[0]);
        }
        pthread_t unknown;assert(!pthread_create(&unknown,NULL,uncached,NULL));assert(!pthread_join(unknown,NULL));
        puts("CPU profile attribution/protected frames/tracking/restarted read/thread retirement PASS");
    }
    if(argc>1 && !strcmp(argv[1],"window")) {
        while(!atomic_load(&stop_requested) && atomic_load(&active)){result=hot_alpha(1000000);}
    } else assert(!raise(SIGUSR1));
    runtime_cpu_profile_stop();assert(!atomic_load(&active));
    return 0;
}
