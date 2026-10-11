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
static void test_fault_dump_config(void) {
    assert(fault_dump_configure(NULL)==0 && !fault_dump_enabled);
    assert(fault_dump_configure("0")==0 && !fault_dump_enabled);
    assert(fault_dump_configure("1")==0 && fault_dump_enabled && !fault_dump_expr_count);
    assert(fault_dump_configure(" r9 + 0x38 , RCX, [r11+0x18], [[r9]+0x38]")==0);
    assert(fault_dump_expr_count==4);
    const char *invalid[]={"foo", "r9,", "r9+", "[r9", "r9+0x10000000000000000",
                           "r9*2", "[[[[[r9]]]]]", "r9+0x"};
    for (unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) {
        assert(fault_dump_configure(invalid[i])==-1);
        assert(!fault_dump_enabled);
    }
    assert(fault_dump_configure("0")==0);
}
static void test_move_register(unsigned char **code, unsigned reg, uint64_t value) {
    *(*code)++=(unsigned char)(reg<8 ? 0x48 : 0x49);
    *(*code)++=(unsigned char)(0xb8+(reg&7));
    memcpy(*code,&value,8); *code+=8;
}
static void test_guest_fault_dump(int enabled) {
    int output[2];
    assert(pipe(output)==0);
    pid_t child=fork();
    assert(child>=0);
    if (!child) {
        close(output[0]); assert(dup2(output[1],2)==2); close(output[1]);
        gpu_enabled=0;
        runtime_fault_recover=NULL;
        assert(fault_dump_configure(enabled ? "r9,[r9],[[r9]+0x38],r14+0xb3c0,r8,[r8],r10+1,rax-1,rbp,rsp,rcx" : "0")==0);
        mach_vm_address_t address=UINT64_C(0x0800000000);
        assert(mach_vm_allocate(mach_task_self(),&address,0x10000,VM_FLAGS_FIXED)==KERN_SUCCESS);
        unsigned char *memory=(void *)(uintptr_t)address;
        image=memory;
        const uint64_t resource=address+0x400, table=address+0x800;
        memcpy(memory+0x100,&resource,8);
        memset(memory+0x400,0x51,128);
        memcpy(memory+0x438,&table,8);
        memset(memory+0x800,0xa6,128);
        memset(memory+0xd3c0,0x64,128);
        unsigned char *code=memory+0xe000;
        test_move_register(&code,0,0);
        test_move_register(&code,1,table);
        test_move_register(&code,8,address+0x20000);
        test_move_register(&code,9,address+0x100);
        test_move_register(&code,10,UINT64_MAX);
        test_move_register(&code,11,address+0xd3c0);
        test_move_register(&code,14,address+0x2000);
        *code++=0xc6; *code++=0x00; *code++=0x07; /* mov byte ptr [rax], 7 */
        assert(mprotect(memory+0xe000,0x1000,PROT_READ|PROT_EXEC)==0);
        struct sigaction action={0}; action.sa_sigaction=fault; action.sa_flags=SA_SIGINFO;
        assert(sigaction(SIGBUS,&action,NULL)==0 && sigaction(SIGSEGV,&action,NULL)==0);
        ((void (*)(void))(memory+0xe000))();
        _exit(1);
    }
    close(output[1]);
    char report[32768]; size_t size=0; ssize_t count;
    while (size<sizeof(report)-1 && (count=read(output[0],report+size,sizeof(report)-1-size))>0)
        size+=(size_t)count;
    report[size]=0; close(output[0]);
    int status;
    assert(waitpid(child,&status,0)==child);
    assert(WIFEXITED(status) && (WEXITSTATUS(status)==128+SIGSEGV || WEXITSTATUS(status)==128+SIGBUS));
    assert(strstr(report,"Guest fault"));
    if (enabled) {
        for (unsigned reg=0;reg<FD_IMAGE;++reg) {
            char expected[64]; snprintf(expected,sizeof(expected),"BB_FAULT_DUMP %s=0x",fault_dump_reg_names[reg]);
            assert(strstr(report,expected));
        }
        assert(strstr(report,"BB_FAULT_DUMP memory [r9] address=0x0000000800000400"));
        assert(strstr(report,"BB_FAULT_DUMP memory [[r9]+0x38] address=0x0000000800000800"));
        assert(strstr(report,"BB_FAULT_DUMP memory r14+0xb3c0 address=0x000000080000d3c0"));
        assert(strstr(report,"BB_FAULT_DUMP memory rsp-stack (256 bytes) address=0x"));
        assert(strstr(report,"51 51 51 51") && strstr(report,"a6 a6 a6 a6") && strstr(report,"64 64 64 64"));
        assert(strstr(report,"unreadable mach=0x"));
        assert(strstr(report,"expression [r8] unreadable dereference"));
        assert(strstr(report,"expression r10+1 address overflow"));
        assert(strstr(report,"expression rax-1 address overflow"));
    } else assert(!strstr(report,"BB_FAULT_DUMP"));
}
int main(void) {
    size_t size=(size_t)sysconf(_SC_PAGESIZE);
    fault_page_size=size;
    assert(fault_dump_configure("1")==0);
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
    test_fault_dump_config();
    test_guest_fault_dump(0);
    test_guest_fault_dump(1);
    puts("Darwin fault recovery, write tracking, watchdog and safe dump tests passed");
    return 0;
}

#else
int main(void) { return 0; }
#endif
