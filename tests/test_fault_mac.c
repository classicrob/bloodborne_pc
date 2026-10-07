#ifdef __APPLE__
#define main bb_probe_main
#include "../src/probe.c"
#undef main
#include <assert.h>
#include <sys/wait.h>
__thread sigjmp_buf *runtime_fault_recover;
static volatile sig_atomic_t write_handled;
static size_t fault_page_size;
int bbgpu_handle_fault(void *context, void *address) {
    ucontext_t *uc=context;
    if (!(uc->uc_mcontext->__es.__err & 2)) return 0;
    if (mprotect((void *)((uintptr_t)address & ~(fault_page_size-1)),fault_page_size,PROT_READ|PROT_WRITE)) return 0;
    write_handled=1;
    return 1;
}
void bbgpu_dump_guest_writes(void *context) { (void)context; }
static void *idle_thread(void *unused) {
    (void)unused;
    for (;;) { struct timespec delay={0,1000000}; nanosleep(&delay,NULL); }
    return NULL;
}
static void test_watchdog(void) {
    int output[2];
    assert(pipe(output)==0);
    pid_t child=fork();
    assert(child>=0);
    if (!child) {
        close(output[0]); assert(dup2(output[1],2)==2); close(output[1]);
        pthread_t other;
        assert(pthread_create(&other,NULL,idle_thread,NULL)==0);
        struct sigaction action={0}; action.sa_sigaction=watchdog; action.sa_flags=SA_SIGINFO;
        assert(sigaction(SIGALRM,&action,NULL)==0);
        raise(SIGALRM);
        _exit(1);
    }
    close(output[1]);
    char report[32768]; size_t size=0;
    ssize_t count;
    while (size<sizeof(report)-1 && (count=read(output[0],report+size,sizeof(report)-1-size))>0) size+=(size_t)count;
    report[size]=0; close(output[0]);
    int status;
    assert(waitpid(child,&status,0)==child);
    assert(WIFEXITED(status) && WEXITSTATUS(status)==128+SIGALRM);
    assert(strstr(report,"watchdog timeout"));
    unsigned threads=0;
    for (char *at=report;(at=strstr(at," rdi="));++at) ++threads;
    assert(threads>=2);
}
int main(void) {
    size_t size=(size_t)sysconf(_SC_PAGESIZE);
    fault_page_size=size;
    gpu_enabled=1;
    volatile unsigned char *address=mmap(NULL,size,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(address!=MAP_FAILED);
    struct sigaction action={0};
    action.sa_sigaction=fault; action.sa_flags=SA_SIGINFO;
    assert(sigaction(SIGBUS,&action,NULL)==0);
    assert(sigaction(SIGSEGV,&action,NULL)==0);
    for (int i=0;i<2;++i) {
        sigjmp_buf recover;
        if (sigsetjmp(recover,1)==0) {
            runtime_fault_recover=&recover;
            unsigned char value=*address;
            (void)value;
            assert(0);
        }
        assert(runtime_fault_recover==NULL);
        sigset_t mask;
        assert(pthread_sigmask(SIG_SETMASK,NULL,&mask)==0);
        assert(!sigismember(&mask,SIGBUS));
        assert(!sigismember(&mask,SIGSEGV));
    }
    assert(mprotect((void *)address,size,PROT_READ)==0);
    *address=42;
    assert(write_handled && *address==42);
    assert(munmap((void *)address,size)==0);
    test_watchdog();
    puts("Darwin fault recovery, write tracking and watchdog tests passed");
    return 0;
}

#else
int main(void) { return 0; }
#endif
