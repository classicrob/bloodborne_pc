/* SDK-only x86_64/Rosetta feasibility probes. Each probe runs in a child. */
#include <cpuid.h>
#include <errno.h>
#include <fcntl.h>
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
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sys/ucontext.h>
#if !defined(__APPLE__) || !defined(__x86_64__)
#error Build with clang -arch x86_64 against the macOS SDK.
#endif
#define GIB (UINT64_C(1)<<30)
#define LOW (448*GIB)
#define HIGH (1024*GIB)
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %s:%d: %s (errno=%d %s)\n",__FILE__,__LINE__,#x,errno,strerror(errno)); exit(1); } } while (0)
static size_t page;
static void *map_at(uintptr_t at, size_t n, int prot) {
    void *p=mmap((void *)at,n,prot,MAP_PRIVATE|MAP_ANON|(at ? MAP_FIXED : 0),-1,0);
    CHECK(p!=MAP_FAILED); return p;
}
static void reserve(void) {
    mach_vm_address_t at=LOW;
    CHECK(mach_vm_allocate(mach_task_self(),&at,HIGH-LOW,VM_FLAGS_FIXED)==KERN_SUCCESS);
    CHECK(map_at(LOW,HIGH-LOW,PROT_NONE)==(void *)LOW);
    printf("reserved [0x%llx, 0x%llx): 576 GiB PROT_NONE\n",(unsigned long long)LOW,(unsigned long long)HIGH);
}
static void reservation_survey(void) {
    void *whole=mmap((void *)(64*GIB),HIGH-64*GIB,PROT_NONE,MAP_ANON|MAP_PRIVATE|MAP_FIXED,-1,0);
    printf("whole [64 GiB,1 TiB): %s errno=%d\n",whole==MAP_FAILED ? "rejected" : "accepted",whole==MAP_FAILED ? errno : 0);
    if (whole!=MAP_FAILED) CHECK(munmap(whole,HIGH-64*GIB)==0);
    for (uintptr_t n=4;n<1024;n++) {
        mach_vm_address_t at=n*GIB;
        kern_return_t kr=mach_vm_allocate(mach_task_self(),&at,GIB,VM_FLAGS_FIXED);
        if (kr!=KERN_SUCCESS) {
            printf("reserve [%lu,%lu) GiB: occupied/protected (Mach %d)\n",n,n+1,kr);
            continue;
        }
        void *p=mmap((void *)(n*GIB),GIB,PROT_NONE,MAP_ANON|MAP_PRIVATE|MAP_FIXED,-1,0);
        printf("reserve [%lu,%lu) GiB: %s errno=%d\n",n,n+1,p==MAP_FAILED ? "rejected" : "accepted",p==MAP_FAILED ? errno : 0);
        CHECK(mach_vm_deallocate(mach_task_self(),at,GIB)==KERN_SUCCESS);
    }
}
static pthread_key_t keys[4];
static uintptr_t (*loads[4])(void);
static void check_tls(uintptr_t tag) {
    for (unsigned i=0;i<4;i++) {
        uintptr_t value=tag+i;
        CHECK(pthread_setspecific(keys[i],(void *)value)==0);
        uintptr_t got=loads[i]();
        printf("TLS thread=%#lx key=%lu disp=%lu got=%#lx expected=%#lx\n",tag,(unsigned long)keys[i],8*(unsigned long)keys[i],got,value);
        CHECK(got==value); CHECK(pthread_getspecific(keys[i])==(void *)value);
    }
}
static void *tls_worker(void *p) { check_tls((uintptr_t)p); return NULL; }
static void tls(void) {
    unsigned char *code=map_at(0,page,PROT_READ|PROT_WRITE);
    for (unsigned i=0;i<4;i++) {
        CHECK(pthread_key_create(&keys[i],NULL)==0);
        const unsigned char op[]={0x65,0x48,0x8b,0x04,0x25,0,0,0,0,0xc3};
        uint32_t disp=(uint32_t)(8*keys[i]);
        memcpy(code+i*16,op,sizeof(op)); memcpy(code+i*16+5,&disp,4);
        loads[i]=(uintptr_t (*)(void))(code+i*16);
    }
    CHECK(mprotect(code,page,PROT_READ|PROT_EXEC)==0);
    check_tls(0x12340000);
    pthread_t workers[3];
    for (unsigned i=0;i<3;i++) CHECK(pthread_create(&workers[i],NULL,tls_worker,(void *)(uintptr_t)(0x23450000+i*0x100))==0);
    for (unsigned i=0;i<3;i++) CHECK(pthread_join(workers[i],NULL)==0);
    for (unsigned i=0;i<4;i++) CHECK(loads[i]()==0x12340000+i);
    puts("9-byte GS load and per-thread isolation passed");
}
static int shared_fd(void) {
    char name[32]; snprintf(name,sizeof(name),"/bb-spike-%d",getpid());
    int fd=shm_open(name,O_CREAT|O_EXCL|O_RDWR,0600); CHECK(fd>=0);
    CHECK(shm_unlink(name)==0); CHECK(ftruncate(fd,65536)==0); return fd;
}
static void aliases(void) {
    reserve(); int fd=shared_fd();
    uintptr_t addresses[]={LOW,640*GIB,768*GIB,HIGH-65536};
    for (unsigned i=0;i<4;i++) CHECK(mmap((void *)addresses[i],65536,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_FIXED,fd,0)==(void *)addresses[i]);
    volatile uint64_t *a=(void *)addresses[0]; *a=0x123456789abcdef0;
    for (unsigned i=0;i<4;i++) { CHECK(*(volatile uint64_t *)addresses[i]==*a); printf("shared alias %#lx verified\n",addresses[i]); }
    CHECK(mprotect((void *)(LOW+16384),16384,PROT_READ)==0);
    CHECK(mprotect((void *)(LOW+4096),4096,PROT_READ)==0);
    CHECK(mprotect((void *)(LOW+4096),4096,PROT_READ|PROT_WRITE)==0);
    *(volatile uint64_t *)(LOW+4096)=42;
    CHECK(*(volatile uint64_t *)(addresses[3]+4096)==42);
    CHECK(mmap((void *)(896*GIB),4096,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_FIXED,fd,4096)==(void *)(896*GIB));
    CHECK(*(volatile uint64_t *)(896*GIB)==42);
    puts("16 KiB and 4 KiB mprotect; 4 KiB shared offset mapping passed"); close(fd);
}
static volatile sig_atomic_t seen_signal, seen_code, fault_count, clobber_vectors;
static volatile uintptr_t seen_rip, seen_address, seen_err;
static void *fault_page;
static size_t fault_size;
static void fault_handler(int sig, siginfo_t *info, void *context) {
    ucontext_t *uc=context;
    seen_signal=sig; seen_code=info->si_code; seen_err=uc->uc_mcontext->__es.__err;
    seen_rip=uc->uc_mcontext->__ss.__rip; seen_address=(uintptr_t)info->si_addr;
    if (++fault_count>1 || mprotect(fault_page,fault_size,PROT_READ|PROT_WRITE)) _exit(91);
    if (clobber_vectors) __asm__ volatile("vzeroall" ::: "xmm0","xmm1","xmm2","xmm3","xmm4","xmm5","xmm6","xmm7","xmm8","xmm9","xmm10","xmm11","xmm12","xmm13","xmm14","xmm15");
}
static void install_handler(void) {
    struct sigaction sa={0}; sa.sa_sigaction=fault_handler; sa.sa_flags=SA_SIGINFO;
    sigemptyset(&sa.sa_mask); CHECK(sigaction(SIGSEGV,&sa,NULL)==0); CHECK(sigaction(SIGBUS,&sa,NULL)==0);
}
/* Defined in assembly so the saved RIP can be checked against an exact label. */
extern void fault_store(void *p);
extern const unsigned char fault_store_instruction[];
__asm__(".text\n.globl _fault_store\n_fault_store:\n.globl _fault_store_instruction\n_fault_store_instruction:\nmovb $42,(%rdi)\nret\n");
static void protection(void) {
    reserve(); install_handler();
    unsigned char *p=map_at(LOW,65536,PROT_READ|PROT_WRITE);
    for (size_t n=4096;n<=16384;n*=4) {
        fault_page=p+n; fault_size=n; fault_count=0; seen_signal=0;
        CHECK(mprotect(fault_page,n,PROT_READ)==0);
        p[0]=1; p[n-1]=2; p[n*2]=3;
        fault_store(fault_page);
        printf("protect %zu: signal=%d (%s) code=%d RIP=%#lx expected=%p address=%#lx resumed=%u\n",n,seen_signal,strsignal(seen_signal),seen_code,seen_rip,fault_store_instruction,seen_address,p[n]);
        printf("Darwin __es.__err=%#lx write-bit=%lu\n",seen_err,(seen_err>>1)&1);
        CHECK(fault_count==1 && seen_rip==(uintptr_t)fault_store_instruction && seen_address==(uintptr_t)fault_page && p[n]==42);
    }
}
static void emit_return(unsigned char *p, uint32_t value) { p[0]=0xb8; memcpy(p+1,&value,4); p[5]=0xc3; }
static void executable(int shared, int rwx, int alias_patch) {
    reserve(); int fd=shared ? shared_fd() : -1;
    unsigned char *p=shared ? mmap((void *)LOW,65536,PROT_READ|PROT_WRITE|(rwx ? PROT_EXEC : 0),MAP_SHARED|MAP_FIXED,fd,0) : map_at(LOW,65536,PROT_READ|PROT_WRITE|(rwx ? PROT_EXEC : 0));
    CHECK(p!=MAP_FAILED);
    unsigned char *writer=p;
    if (alias_patch) { writer=mmap((void *)(640*GIB),65536,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_FIXED,fd,0); CHECK(writer!=MAP_FAILED); }
    emit_return(writer,17);
    if (!rwx) CHECK(mprotect(p,65536,PROT_READ|PROT_EXEC)==0);
    int (*fn)(void)=(int (*)(void))p; CHECK(fn()==17);
    for (unsigned i=18;i<118;i++) {
        if (!rwx && !alias_patch) CHECK(mprotect(p,65536,PROT_READ|PROT_WRITE)==0);
        emit_return(writer,i);
        if (!rwx && !alias_patch) CHECK(mprotect(p,65536,PROT_READ|PROT_EXEC)==0);
        int got=fn();
        if (got!=(int)i) { printf("stale code got=%d expected=%u\n",got,i); exit(1); }
    }
    printf("%s %s patch via %s: 101 executions passed, no explicit cache flush\n",shared ? "shared" : "anonymous",rwx ? "RWX" : "RW->RX",alias_patch ? "RW alias" : "same view");
}
static void exec_rwx(void) { executable(0,1,0); }
static void exec_rx(void) { executable(0,0,0); }
static void exec_shared_rwx(void) { executable(1,1,0); }
static void exec_shared_rx(void) { executable(1,0,0); }
static void exec_alias(void) { executable(1,0,1); }
static void allocations(void) {
    const size_t sizes[]={16,256,4096,16384,1048576,16777216};
    void *p[6];
    for (unsigned i=0;i<6;i++) { p[i]=malloc(sizes[i]); CHECK(p[i]); printf("malloc(%zu)=%p below 1 TiB=%s\n",sizes[i],p[i],(uintptr_t)p[i]<HIGH ? "yes" : "no"); }
    for (unsigned i=0;i<6;i++) free(p[i]);
    void *hint=mmap((void *)LOW,65536,PROT_READ|PROT_WRITE,MAP_ANON|MAP_PRIVATE,-1,0);
    CHECK(hint!=MAP_FAILED); printf("mmap hint=%p below 1 TiB=%s\n",hint,(uintptr_t)hint<HIGH ? "yes" : "no"); munmap(hint,65536);
    void *fixed=map_at(LOW,65536,PROT_READ|PROT_WRITE); *(volatile char *)fixed=42;
    printf("explicit low mmap=%p writable=%d\n",fixed,*(volatile char *)fixed);
}
static void cpuid(void) {
    unsigned a,b,c,d; __cpuid_count(1,0,a,b,c,d);
    printf("CPUID.1 ecx=%08x SSE4.1=%u SSE4.2=%u AES=%u POPCNT=%u AVX=%u F16C=%u OSXSAVE=%u\n",c,(c>>19)&1,(c>>20)&1,(c>>25)&1,(c>>23)&1,(c>>28)&1,(c>>29)&1,(c>>27)&1);
    if (c&(1u<<27)) { unsigned lo,hi; __asm__ volatile("xgetbv":"=a"(lo),"=d"(hi):"c"(0)); printf("XCR0=%08x%08x\n",hi,lo); }
    __cpuid_count(7,0,a,b,c,d); printf("CPUID.7 ebx=%08x BMI1=%u AVX2=%u\n",b,(b>>3)&1,(b>>5)&1);
    __cpuid_count(0x80000001,0,a,b,c,d); printf("CPUID.80000001 ecx=%08x LZCNT=%u SSE4a=%u\n",c,(c>>5)&1,(c>>6)&1);
}
static void avx(void) { float out[8]; __asm__ volatile("vxorps %%ymm0,%%ymm0,%%ymm0; vcmpps $0,%%ymm0,%%ymm0,%%ymm0; vandps %%ymm0,%%ymm0,%%ymm0; vmovups %%ymm0,%0; vzeroupper":"=m"(out)::"xmm0"); for(unsigned i=0;i<8;i++) CHECK(((uint32_t *)out)[i]==UINT32_MAX); }
static void f16c(void) { float out[4]; uint16_t in[4]={0x3c00,0x4000,0x4200,0x4400}; __asm__ volatile("vcvtph2ps %1,%%xmm0; movups %%xmm0,%0":"=m"(out):"m"(in):"xmm0"); for(unsigned i=0;i<4;i++) CHECK(out[i]==(float)(i+1)); }
static void bmi1(void) { unsigned long out,in=0x100; __asm__ volatile("blsi %1,%0":"=r"(out):"r"(in):"cc"); CHECK(out==0x100); }
static void sse41(void) { int out; __asm__ volatile("pcmpeqd %%xmm0,%%xmm0; ptest %%xmm0,%%xmm0; mov $0,%0; setnz %b0":"=r"(out)::"xmm0","cc"); CHECK(out==1); }
static void sse42(void) { unsigned out=0,in=0x12345678; __asm__ volatile("crc32l %1,%0":"+r"(out):"r"(in):"cc"); CHECK(out==0xfa745634); }
static void aes(void) { unsigned char out[16]; __asm__ volatile("pxor %%xmm0,%%xmm0; aesenc %%xmm0,%%xmm0; movdqu %%xmm0,%0":"=m"(out)::"xmm0"); for(unsigned i=0;i<16;i++) CHECK(out[i]==0x63); }
static void popcnt(void) { unsigned long out,in=0xf001; __asm__ volatile("popcnt %1,%0":"=r"(out):"r"(in):"cc"); CHECK(out==5); }
static void lzcnt(void) { unsigned long out,in=1; __asm__ volatile("lzcnt %1,%0":"=r"(out):"r"(in):"cc"); CHECK(out==63); }
static void sse4a_extrq(void) {
    uint64_t in[2]={0x123456789abcdef0,0},out[2];
    __asm__ volatile("movdqu %1,%%xmm0; extrq $8,$8,%%xmm0; movdqu %%xmm0,%0":"=m"(out):"m"(in):"xmm0");
    CHECK(out[0]==0xde);
}
static void sse4a_insertq(void) {
    uint64_t in[2]={0x123456789abcdef0,0},insert[2]={0x55,0},out[2];
    __asm__ volatile("movdqu %1,%%xmm0; movdqu %2,%%xmm1; insertq $8,$8,%%xmm1,%%xmm0; movdqu %%xmm0,%0":"=m"(out):"m"(in),"m"(insert):"xmm0","xmm1");
    CHECK(out[0]==0x123456789abc55f0);
}
static void mach_ranges(void) {
    const uintptr_t starts[]={4*GIB,32*GIB,64*GIB,128*GIB,256*GIB,448*GIB};
    for (unsigned i=0;i<sizeof(starts)/sizeof(*starts);i++) {
        mach_vm_address_t at=starts[i];
        kern_return_t kr=mach_vm_allocate(mach_task_self(),&at,65536,VM_FLAGS_FIXED);
        printf("mach_vm_allocate %#lx: %d (%s)\n",starts[i],kr,mach_error_string(kr));
        if (kr==KERN_SUCCESS) CHECK(mach_vm_deallocate(mach_task_self(),at,65536)==KERN_SUCCESS);
    }
}
static pthread_mutex_t state_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t state_cond=PTHREAD_COND_INITIALIZER;
static int state_ready,state_done;
static void *state_worker(void *arg) {
    (void)arg; pthread_mutex_lock(&state_lock); state_ready=1; pthread_cond_signal(&state_cond);
    while (!state_done) pthread_cond_wait(&state_cond,&state_lock);
    pthread_mutex_unlock(&state_lock); return NULL;
}
static void mach_thread_state(void) {
    x86_thread_state64_t state; mach_msg_type_number_t n=x86_THREAD_STATE64_COUNT;
    thread_t self=mach_thread_self();
    kern_return_t kr=thread_get_state(self,x86_THREAD_STATE64,(thread_state_t)&state,&n);
    printf("thread_get_state self x86_THREAD_STATE64: %d (%s) RIP=%#llx\n",kr,mach_error_string(kr),kr==KERN_SUCCESS ? state.__rip : 0);
    mach_port_deallocate(mach_task_self(),self); CHECK(kr==KERN_SUCCESS);
    pthread_t worker; CHECK(pthread_create(&worker,NULL,state_worker,NULL)==0);
    pthread_mutex_lock(&state_lock);
    while (!state_ready) pthread_cond_wait(&state_cond,&state_lock);
    n=x86_THREAD_STATE64_COUNT;
    kr=thread_get_state(pthread_mach_thread_np(worker),x86_THREAD_STATE64,(thread_state_t)&state,&n);
    printf("thread_get_state other pthread: %d (%s) RIP=%#llx\n",kr,mach_error_string(kr),kr==KERN_SUCCESS ? state.__rip : 0);
    state_done=1; pthread_cond_signal(&state_cond); pthread_mutex_unlock(&state_lock);
    CHECK(pthread_join(worker,NULL)==0); CHECK(kr==KERN_SUCCESS);
}
extern void avx_fault_store(void *,void *,const void *);
extern const unsigned char avx_fault_instruction[];
__asm__(".text\n.globl _avx_fault_store\n_avx_fault_store:\n"
        " vmovdqu (%rdx),%ymm0\n"
        " vmovdqu (%rdx),%ymm1\n"
        " vmovdqu (%rdx),%ymm2\n"
        " vmovdqu (%rdx),%ymm3\n"
        " vmovdqu (%rdx),%ymm4\n"
        " vmovdqu (%rdx),%ymm5\n"
        " vmovdqu (%rdx),%ymm6\n"
        " vmovdqu (%rdx),%ymm7\n"
        " vmovdqu (%rdx),%ymm8\n"
        " vmovdqu (%rdx),%ymm9\n"
        " vmovdqu (%rdx),%ymm10\n"
        " vmovdqu (%rdx),%ymm11\n"
        " vmovdqu (%rdx),%ymm12\n"
        " vmovdqu (%rdx),%ymm13\n"
        " vmovdqu (%rdx),%ymm14\n"
        " vmovdqu (%rdx),%ymm15\n"
        ".globl _avx_fault_instruction\n_avx_fault_instruction:\n movb $42,(%rdi)\n"
        " vmovdqu %ymm0,0(%rsi)\n"
        " vmovdqu %ymm1,32(%rsi)\n"
        " vmovdqu %ymm2,64(%rsi)\n"
        " vmovdqu %ymm3,96(%rsi)\n"
        " vmovdqu %ymm4,128(%rsi)\n"
        " vmovdqu %ymm5,160(%rsi)\n"
        " vmovdqu %ymm6,192(%rsi)\n"
        " vmovdqu %ymm7,224(%rsi)\n"
        " vmovdqu %ymm8,256(%rsi)\n"
        " vmovdqu %ymm9,288(%rsi)\n"
        " vmovdqu %ymm10,320(%rsi)\n"
        " vmovdqu %ymm11,352(%rsi)\n"
        " vmovdqu %ymm12,384(%rsi)\n"
        " vmovdqu %ymm13,416(%rsi)\n"
        " vmovdqu %ymm14,448(%rsi)\n"
        " vmovdqu %ymm15,480(%rsi)\n"
        " vzeroupper\n ret\n");
static void avx_fault_state(void) {
    install_handler();
    fault_size=4096; fault_page=map_at(0,4096,PROT_READ); clobber_vectors=1;
    uint32_t pattern[8]={0x12345678,0xabcdef01,0x31415926,0,0x80000000,0x98765432,0xffffffff,0x10203040};
    uint32_t out[16][8]; memset(out,0,sizeof(out));
    avx_fault_store(fault_page,out,pattern);
    CHECK(fault_count==1 && seen_rip==(uintptr_t)avx_fault_instruction);
    for(unsigned i=0;i<16;i++) CHECK(!memcmp(out[i],pattern,sizeof(pattern)));
    puts("All 16 YMM registers retain 256 bits across SIGBUS/mprotect/retry despite vzeroall in handler");
}
static void bmi1_bextr(void) {
    uint64_t in=0x123456789abcdef0,control=0x0808,out;
    unsigned char z;
    __asm__ volatile("bextr %3,%2,%0; setz %1":"=r"(out),"=qm"(z):"r"(in),"r"(control):"cc");
    CHECK(out==0xde && z==0);
}
static int run(const char *name,void (*fn)(void)) {
    printf("\n=== %s ===\n",name); fflush(NULL);
    pid_t pid=fork(); CHECK(pid>=0);
    if (!pid) { alarm(20); fn(); fflush(NULL); _exit(0); }
    int status; CHECK(waitpid(pid,&status,0)==pid);
    if (WIFSIGNALED(status)) printf("RESULT %s: SIGNAL %d (%s)\n",name,WTERMSIG(status),strsignal(WTERMSIG(status)));
    else printf("RESULT %s: %s (exit %d)\n",name,WEXITSTATUS(status) ? "FAIL" : "PASS",WEXITSTATUS(status));
    return !WIFEXITED(status) || WEXITSTATUS(status)!=0;
}
int main(int argc,char **argv) {
    if (argc>1 && !strcmp(argv[1],"--vmmap")) { printf("pid=%d\n",getpid()); fflush(stdout); pause(); return 0; }
    setvbuf(stdout,NULL,_IOLBF,0); struct rlimit lim={0,0}; setrlimit(RLIMIT_CORE,&lim);
    int translated=0; size_t len=sizeof(translated); CHECK(sysctlbyname("sysctl.proc_translated",&translated,&len,NULL,0)==0);
    struct utsname u; CHECK(uname(&u)==0); page=(size_t)sysconf(_SC_PAGESIZE);
    printf("Darwin=%s machine=%s Rosetta=%d page=%zu\n",u.release,u.machine,translated,page); CHECK(translated==1);
    int failed=0;
#define RUN(f) failed+=run(#f,f)
    RUN(tls); RUN(reservation_survey); RUN(mach_ranges); RUN(mach_thread_state); RUN(aliases); RUN(protection); RUN(exec_rwx); RUN(exec_rx); RUN(exec_shared_rwx); RUN(exec_shared_rx); RUN(exec_alias); RUN(allocations); RUN(cpuid);
    RUN(avx); RUN(avx_fault_state); RUN(f16c); RUN(bmi1); RUN(bmi1_bextr); RUN(sse41); RUN(sse42); RUN(aes); RUN(popcnt); RUN(lzcnt); RUN(sse4a_extrq); RUN(sse4a_insertq);
    printf("\n%d probes failed\n",failed); return failed ? 1 : 0;
}
