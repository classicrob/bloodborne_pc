#define _GNU_SOURCE
#define BB_HLE_TRACE_TEST
#include "../src/runtime_hle_trace.c"
#include <assert.h>
#include <setjmp.h>
#include <stdarg.h>
#include <sys/wait.h>
#include <sys/resource.h>
static _Thread_local int32_t guest_error;
static int interrupt_publication;
static volatile sig_atomic_t dump_seen;
static int fatal_publication;
int32_t *runtime_errno(void) { return &guest_error; }
void *runtime_low_map_named(size_t n,int prot,const char *owner) {
    (void)owner;void *p=mmap(NULL,n,prot,MAP_PRIVATE|MAP_ANON,-1,0);
    return p==MAP_FAILED ? NULL : p;
}
static void signal_dump(int sig) { runtime_hle_trace_dump();dump_seen=1;if(sig==SIGSEGV||sig==SIGBUS)_exit(128+sig); }
void hle_trace_publication_hook(void) {
    if(fatal_publication){volatile int *p=mmap(NULL,4096,PROT_NONE,MAP_PRIVATE|MAP_ANON,-1,0);*p=1;}
    if(interrupt_publication){interrupt_publication=0;raise(SIGUSR1);}
}
static uint64_t many(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f,
                     uint64_t g,uint64_t h,uint64_t i,uint64_t j) {
    assert(errno==17 && guest_error==23);errno=29;guest_error=31;
    return a+2*b+3*c+4*d+5*e+6*f+7*g+8*h+9*i+10*j;
}
static double varargs(unsigned n,...) {
    va_list args;va_start(args,n);double sum=0;
    for(unsigned i=0;i<n;i++)sum+=va_arg(args,double);
    va_end(args);return sum;
}
static long double extended(long double n) { return n*3.0L+0.125L; }
typedef struct { uint64_t x,y; } Pair;
static Pair pair(uint64_t n){return (Pair){n,~n};}
typedef float V8 __attribute__((vector_size(32)));
static V8 vector(V8 a,V8 b){return a+b;}
static uint64_t simple(uint64_t n){return n+1;}
static uint64_t (*inner)(uint64_t);
static uint64_t outer(uint64_t n){return inner(n)+1;}
static jmp_buf recovery;
static uint64_t jump_target(uint64_t n){longjmp(recovery,(int)n);}
static uint64_t (*jump_call)(uint64_t);
static uint64_t recover_outer(uint64_t n){
    for(unsigned i=0;i<8;i++)if(!setjmp(recovery))jump_call(n);
    return n+40;
}
static void *worker(void *unused) {
    assert(!pthread_setname_np("hle-summary-worker"));
    (void)unused;for(unsigned i=0;i<3000;i++)assert(inner(i)==i+1);assert(!depth);return NULL;
}
static pthread_mutex_t held_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t held_cv=PTHREAD_COND_INITIALIZER;
static int held_entered,held_release;
static uint64_t held_target(uint64_t n) {
    pthread_mutex_lock(&held_lock);held_entered=1;pthread_cond_broadcast(&held_cv);
    while(!held_release)pthread_cond_wait(&held_cv,&held_lock);
    pthread_mutex_unlock(&held_lock);return n;
}
static uint64_t (*held_call)(uint64_t);
static void *held_worker(void *unused) { (void)unused;assert(held_call(42)==42);return NULL; }
static unsigned imp_index(const char *name) {
    for(unsigned i=0;i<import_count;i++)if(!strcmp(imports[i].name,name))return i;
    abort();
}
static _Thread_local jmp_buf retirement_jump;
static uint64_t exit_target(uint64_t n) { pthread_exit((void *)(uintptr_t)n); }
static uint64_t retire_jump_target(uint64_t n) { (void)n;longjmp(retirement_jump,1); }
static uint64_t (*retire_call)(uint64_t);
static void *retire_worker(void *unused) {
    (void)unused;
    if(!setjmp(retirement_jump))retire_call(42);
    return (void *)42;
}
static void retirement_controls(void) {
    const char *names[]={"scePthreadCondWait","scePthreadCondTimedwait"};
    uintptr_t targets[]={(uintptr_t)exit_target,(uintptr_t)retire_jump_target};
    for(unsigned i=0;i<2;i++) {
        retire_call=(void *)runtime_hle_trace_wrap(names[i],names[i],targets[i]);
        pthread_t t;void *result;assert(!pthread_create(&t,NULL,retire_worker,NULL));
        assert(!pthread_join(t,&result)&&result==(void *)42);
        SummaryThread *node=atomic_load(&summary_threads);
        assert(node->retired&&!node->active_import&&!node->active_start);
        SummaryMetric *m=&node->metrics[imp_index(names[i])];
        assert(m->abandoned==1&&!m->inflight&&!m->count);
    }
}
static void summary_controls(void) {
    pthread_t threads[4];for(unsigned i=0;i<4;i++)assert(!pthread_create(&threads[i],NULL,worker,NULL));
    for(unsigned i=0;i<4;i++)assert(!pthread_join(threads[i],NULL));
    unsigned workers=0;uint64_t total=0;
    for(SummaryThread *t=atomic_load(&summary_threads);t;t=t->next) {
        if(!strcmp(t->name,"hle-summary-worker")){workers++;assert(t->metrics[imp_index("simple")].count==3000);}
        for(unsigned i=0;i<TRACE_IMPORTS;i++)assert(!t->metrics[i].inflight);
        total+=t->metrics[imp_index("simple")].count;
    }
    assert(workers==4&&total==12001&&sequence==0);
    assert(summary_thread->metrics[imp_index("recover")].count==15);
    assert(summary_thread->metrics[imp_index("jump")].abandoned==120);
    assert(summary_thread->metrics[imp_index("many")].count==1);
    retirement_controls();
    held_call=(void *)runtime_hle_trace_wrap("held","sceKernelWaitSema",(uintptr_t)held_target);
    pthread_t held;assert(!pthread_create(&held,NULL,held_worker,NULL));
    pthread_mutex_lock(&held_lock);while(!held_entered)pthread_cond_wait(&held_cv,&held_lock);pthread_mutex_unlock(&held_lock);
    usleep(20000);
    FILE *capture=tmpfile();assert(capture);int fd=dup(2);assert(fd>=0&&dup2(fileno(capture),2)>=0);
    errno=37;guest_error=41;runtime_hle_trace_frame();assert(!ftell(capture));runtime_hle_trace_frame();
    assert(errno==37&&guest_error==41);
    pthread_mutex_lock(&held_lock);held_release=1;pthread_cond_broadcast(&held_cv);pthread_mutex_unlock(&held_lock);
    assert(!pthread_join(held,NULL));
    SummaryThread *h=atomic_load(&summary_threads);
    assert(h->metrics[imp_index("sceKernelWaitSema")].count==1);
    assert(h->metrics[imp_index("sceKernelWaitSema")].elapsed_ns>=20000000);
    assert(h->metrics[imp_index("sceKernelWaitSema")].max_ns==h->metrics[imp_index("sceKernelWaitSema")].elapsed_ns);
    runtime_hle_trace_summary();runtime_hle_trace_summary();runtime_hle_trace_dump();
    assert(sequence==0&&dup2(fd,2)>=0);close(fd);rewind(capture);
    char line[1024];int pending=0,completed=0,headers=0,empty=0,retired=0;
    while(fgets(line,sizeof(line),capture)) {
        assert(!strstr(line,"HLE-TRACE"));
        if(strstr(line,"window_ms=")){headers++;empty=0;}
        if(strstr(line,"retired=1")&&strstr(line,"abandoned=1 inflight=0"))retired++;
        if(strstr(line,"active=sceKernelWaitSema"))pending++;
        if(strstr(line,"import=sceKernelWaitSema completed=1"))completed++;
        if(strstr(line,"import="))empty++;
    }
    assert(headers==3&&pending==1&&completed==1&&empty==0&&retired==2);fclose(capture);
    puts("PASS: summary ABI/errno/nonlocal controls, four independent thread counts, no ring, pending wait, interval reset, wall duration/max, frame cadence");
}
static int (*bench_lock)(pthread_mutex_t *),(*bench_unlock)(pthread_mutex_t *);
static int (*bench_clock)(clockid_t,struct timespec *);
static void *broad_worker(void *arg) {
    (void)arg;pthread_mutex_t m=PTHREAD_MUTEX_INITIALIZER;
    for(unsigned i=0;i<100000;i++) {
        struct timespec t;assert(!bench_lock(&m));assert(!bench_clock(CLOCK_MONOTONIC,&t));assert(!bench_unlock(&m));
    }
    assert(!pthread_mutex_destroy(&m));return NULL;
}
static void broad_benchmark(const char *mode) {
    unsetenv("BB_HLE_TRACE");setenv("BB_HLE_TRACE_SUMMARY_FILTER","sync,gnm,videoout,avplayer",1);
    if(!strcmp(mode,"broad-on"))setenv("BB_HLE_TRACE_SUMMARY","600",1);else unsetenv("BB_HLE_TRACE_SUMMARY");
    bench_lock=(void *)runtime_hle_trace_wrap("lock","scePthreadMutexLock",(uintptr_t)pthread_mutex_lock);
    bench_unlock=(void *)runtime_hle_trace_wrap("unlock","scePthreadMutexUnlock",(uintptr_t)pthread_mutex_unlock);
    bench_clock=(void *)runtime_hle_trace_wrap("clock","sceKernelClockGettime",(uintptr_t)clock_gettime);
    struct rusage a,b;assert(!getrusage(RUSAGE_SELF,&a));uint64_t start=monotonic_ns();
    pthread_t threads[4];for(unsigned i=0;i<4;i++)assert(!pthread_create(&threads[i],NULL,broad_worker,NULL));
    for(unsigned i=0;i<4;i++)assert(!pthread_join(threads[i],NULL));
    uint64_t elapsed=monotonic_ns()-start;assert(!getrusage(RUSAGE_SELF,&b));
    double cpu=(b.ru_utime.tv_sec-a.ru_utime.tv_sec+b.ru_stime.tv_sec-a.ru_stime.tv_sec)*1000000.0+b.ru_utime.tv_usec-a.ru_utime.tv_usec+b.ru_stime.tv_usec-a.ru_stime.tv_usec;
    if(summary_frames) {
        unsigned nodes=0;for(SummaryThread *t=atomic_load(&summary_threads);t;t=t->next) {
            nodes++;assert(t->retired);for(unsigned i=0;i<3;i++)assert(t->metrics[i].count==100000&&!t->metrics[i].inflight);
        }
        assert(nodes==4&&!sequence);
    }
    printf("BROAD %s calls=1200000 wall_ms=%.3f cpu_ms=%.3f\n",mode,elapsed/1e6,cpu/1000.0);
}
static void benchmark(const char *mode) {
    if(!strcmp(mode,"on"))setenv("BB_HLE_TRACE_SUMMARY","600",1);else unsetenv("BB_HLE_TRACE_SUMMARY");
    unsetenv("BB_HLE_TRACE");setenv("BB_HLE_TRACE_SUMMARY_FILTER","simple",1);
    uint64_t (*f)(uint64_t)=(void *)runtime_hle_trace_wrap("simple","simple",(uintptr_t)simple);
    struct rusage cpu0,cpu1;assert(!getrusage(RUSAGE_SELF,&cpu0));
    uint64_t n=0,start=monotonic_ns();
    for(unsigned i=0;i<1000000;i++)n=f(n);
    uint64_t elapsed=monotonic_ns()-start;
    assert(!getrusage(RUSAGE_SELF,&cpu1));
    double cpu=1000.0*((cpu1.ru_utime.tv_sec-cpu0.ru_utime.tv_sec+cpu1.ru_stime.tv_sec-cpu0.ru_stime.tv_sec)*1000000.0+cpu1.ru_utime.tv_usec-cpu0.ru_utime.tv_usec+cpu1.ru_stime.tv_usec-cpu0.ru_stime.tv_usec);
    assert(n==1000000);printf("BENCH %s calls=1000000 elapsed_ns=%llu ns_per_call=%.3f cpu_ns_per_call=%.3f\n",mode,(unsigned long long)elapsed,elapsed/1000000.0,cpu/1000000.0);
}
int main(int argc,char **argv) {
    alarm(120);
    if(argc>2&&!strcmp(argv[1],"bench")){if(!strncmp(argv[2],"broad-",6))broad_benchmark(argv[2]);else benchmark(argv[2]);return 0;}
    if(argc>1&&!strcmp(argv[1],"default-summary")) {
        unsetenv("BB_HLE_TRACE");unsetenv("BB_HLE_TRACE_SUMMARY_FILTER");setenv("BB_HLE_TRACE_SUMMARY","600",1);
        assert(runtime_hle_trace_wrap("lock","scePthreadMutexLock",(uintptr_t)simple)==(uintptr_t)simple);
        assert(runtime_hle_trace_wrap("clock","sceKernelClockGettime",(uintptr_t)simple)==(uintptr_t)simple);
        assert(runtime_hle_trace_wrap("wait","sceKernelWaitEqueue",(uintptr_t)simple)!=(uintptr_t)simple);
        assert(runtime_hle_trace_wrap("sleep","sceKernelUsleep",(uintptr_t)simple)!=(uintptr_t)simple);
        assert(runtime_hle_trace_wrap("submit","sceGnmSubmitDone",(uintptr_t)simple)!=(uintptr_t)simple);
        puts("PASS: default summary waits/submit filters exclude mutex and clock hot paths");return 0;
    }
    int summary_mode=argc>1&&!strcmp(argv[1],"summary");
    if(argc>1&&!summary_mode){unsetenv("BB_HLE_TRACE");unsetenv("BB_HLE_TRACE_SUMMARY");assert(runtime_hle_trace_wrap("nid","simple",(uintptr_t)simple)==(uintptr_t)simple);puts("PASS: disabled identity");return 0;}
    setenv("BB_HLE_TRACE","sync,gnm,videoout,avplayer,many,varargs,extended,pair,vector,simple,outer,jump,recover",1);
    if(summary_mode){setenv("BB_HLE_TRACE_SUMMARY","2",1);setenv("BB_HLE_TRACE_SUMMARY_FILTER",getenv("BB_HLE_TRACE"),1);unsetenv("BB_HLE_TRACE");}
    pid_t pid=fork();assert(pid>=0);if(!pid){execl(argv[0],argv[0],"off",NULL);_exit(99);}int status;assert(waitpid(pid,&status,0)==pid&&WIFEXITED(status)&&!WEXITSTATUS(status));
    assert(runtime_hle_trace_wrap("other","memcpy",(uintptr_t)simple)==(uintptr_t)simple);
    assert(runtime_hle_trace_wrap("data","sceKernelWaitEqueue",(uintptr_t)simple)!=(uintptr_t)simple);
    uint64_t (*m)(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t)=
        (void *)runtime_hle_trace_wrap("many","many",(uintptr_t)many);
    errno=17;guest_error=23;assert(m(1,2,3,4,5,6,7,8,9,10)==385);assert(errno==29&&guest_error==31);
    double (*v)(unsigned,...)=(void *)runtime_hle_trace_wrap("varargs","varargs",(uintptr_t)varargs);
    assert(v(10,1.,2.,3.,4.,5.,6.,7.,8.,9.,10.)==55.);
    long double (*x)(long double)=(void *)runtime_hle_trace_wrap("extended","extended",(uintptr_t)extended);
    assert(x(1.25L)==3.875L);
    Pair (*p)(uint64_t)=(void *)runtime_hle_trace_wrap("pair","pair",(uintptr_t)pair);Pair r=p(0x1234);assert(r.x==0x1234&&r.y==~UINT64_C(0x1234));
    V8 (*vec)(V8,V8)=(void *)runtime_hle_trace_wrap("vector","vector",(uintptr_t)vector);
    V8 vr=vec((V8){1,2,3,4,5,6,7,8},(V8){8,7,6,5,4,3,2,1});
    for(unsigned i=0;i<8;i++)assert(vr[i]==9);
    inner=(void *)runtime_hle_trace_wrap("simple","simple",(uintptr_t)simple);
    uint64_t (*o)(uint64_t)=(void *)runtime_hle_trace_wrap("outer","outer",(uintptr_t)outer);
    assert(o(7)==9&&!depth);
    jump_call=(void *)runtime_hle_trace_wrap("jump","jump",(uintptr_t)jump_target);
    uint64_t (*recover)(uint64_t)=(void *)runtime_hle_trace_wrap("recover","recover",(uintptr_t)recover_outer);
    for(unsigned i=1;i<16;i++)assert(recover(i)==i+40&&!depth);
    if(summary_mode){summary_controls();return 0;}
    assert(signal(SIGUSR1,signal_dump)!=SIG_ERR);interrupt_publication=1;
    assert(inner(2)==3&&dump_seen&&!depth);
    assert(signal(SIGSEGV,signal_dump)!=SIG_ERR);
    assert(signal(SIGBUS,signal_dump)!=SIG_ERR);
    pid=fork();assert(pid>=0);if(!pid){fatal_publication=1;inner(5);_exit(99);}
    assert(waitpid(pid,&status,0)==pid&&WIFEXITED(status)&&(WEXITSTATUS(status)==128+SIGSEGV||WEXITSTATUS(status)==128+SIGBUS));
    pthread_t threads[4];for(unsigned i=0;i<4;i++)assert(!pthread_create(&threads[i],NULL,worker,NULL));
    for(unsigned i=0;i<4;i++)assert(!pthread_join(threads[i],NULL));
    assert(sequence>TRACE_CAPACITY);
    for(uint64_t i=sequence-TRACE_CAPACITY;i<sequence;i++)assert(records[i%TRACE_CAPACITY].seq==i+1);
    assert(inner(9)==10);
    TraceRecord *last=&records[(sequence-1)%TRACE_CAPACITY];
    assert(last->exit&&last->rax==10&&last->args[0]==9&&last->call==sequence-1);
    int saved_stderr=dup(STDERR_FILENO);assert(saved_stderr>=0);
    FILE *capture=tmpfile();assert(capture);assert(dup2(fileno(capture),STDERR_FILENO)>=0);
    errno=37;runtime_hle_trace_dump();assert(errno==37);
    assert(dup2(saved_stderr,STDERR_FILENO)>=0);close(saved_stderr);
    rewind(capture);char line[1024];unsigned count=0;
    while(fgets(line,sizeof(line),capture))if(!strncmp(line,"HLE-TRACE ENTER ",16)||!strncmp(line,"HLE-TRACE EXIT ",15))count++;
    assert(count==TRACE_CAPACITY);fclose(capture);
    puts("PASS: stack arguments, variadic FP, x87, RAX/RDX aggregate, nested calls, nonlocal outer return/reused slots, errno, interrupted publication, concurrent ring wrap");
    return 0;
}
