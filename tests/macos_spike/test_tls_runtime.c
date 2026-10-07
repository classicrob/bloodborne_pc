/* Exercises the real loader rewrite and guest-thread implementation under Rosetta. */
#define main unused_probe_main
#include "../../src/probe.c"
#undef main
#define thread_create guest_thread_create
#include "../../src/runtime_thread.c"
#undef thread_create
#include <assert.h>
#include <mach/mach_vm.h>
#include <sys/sysctl.h>

static pthread_mutex_t alloc_lock=PTHREAD_MUTEX_INITIALIZER;
static uintptr_t next_low=UINT64_C(0x800000000);
static int fail_after=-1;
void *runtime_low_map(size_t n,int prot) {
    if (fail_after==0) return NULL;
    if (fail_after>0) --fail_after;
    n=(n+4095)&~(size_t)4095;
    pthread_mutex_lock(&alloc_lock);
    mach_vm_address_t p=next_low;
    assert(mach_vm_allocate(mach_task_self(),&p,n,VM_FLAGS_FIXED)==KERN_SUCCESS);
    next_low+=n+4096;
    pthread_mutex_unlock(&alloc_lock);
    assert(mprotect((void *)p,n,prot)==0); return (void *)p;
}
void *runtime_guest_calloc(size_t n,size_t size) {
    if (size && n>SIZE_MAX/size) return NULL;
    return runtime_low_map(n*size,PROT_READ|PROT_WRITE);
}
void runtime_guest_free(void *p) { (void)p; }
void runtime_thread_keys_cleanup(void) {}

static uint32_t offsets[4];
static void *race_key(void *arg) {
    size_t i=(size_t)arg; offsets[i]=runtime_thread_tcb_offset(); return NULL;
}
static uintptr_t (*tcb_load)(void);
static const unsigned char initial_tls[]={1,2,3,4,5,6,7,8};
static uint64_t *check_current(int value) {
    GuestThread *t=runtime_thread_current();
    uint64_t *tcb=(void *)tcb_load();
    assert(tcb==t->tcb && tcb[0]==(uintptr_t)tcb && tcb[2]==(uintptr_t)t);
    assert(tcb[1]==(uintptr_t)t->dtv);
    assert((uintptr_t)t<(UINT64_C(1)<<40));
    assert((uintptr_t)tcb<(UINT64_C(1)<<40) && (uintptr_t)tcb%8192==0);
    assert((uintptr_t)runtime_errno()<(UINT64_C(1)<<40));
    assert(!memcmp((unsigned char *)tcb-8192,initial_tls,sizeof(initial_tls)));
    unsigned char *tls=(unsigned char *)tcb-8192;
    assert(tls[16]==0); tls[16]=(unsigned char)value;
    assert(*runtime_errno()==0); *runtime_errno()=value;
    assert(*guest_error()==value);
    return tcb;
}
static void *host_worker(void *arg) {
    runtime_thread_attach_host("TLS worker");
    return check_current((int)(uintptr_t)arg);
}
static ABI void *guest_worker(void *arg) {
    char on_stack;
    assert((uintptr_t)&on_stack<(UINT64_C(1)<<40));
    assert((void *)thread_self()==current);
    check_current(456); return arg;
}
int main(void) {
    int translated=0; size_t len=sizeof(translated);
    assert(!sysctlbyname("sysctl.proc_translated",&translated,&len,NULL,0) && translated);
    pthread_t races[4];
    for (size_t i=0;i<4;i++) assert(!pthread_create(&races[i],NULL,race_key,(void *)i));
    for (size_t i=0;i<4;i++) { assert(!pthread_join(races[i],NULL)); assert(offsets[i]==offsets[0]); }
    runtime_set_main_tls(initial_tls,sizeof(initial_tls),32,8192);
    const unsigned char load[]={0x65,0x48,0x8b,0x04,0x25,0,0,0,0};
    image=runtime_low_map(4096,PROT_READ|PROT_WRITE);
    memcpy(image,load,9); image[9]=0xc3;
    memcpy(image+16,load,9);
    memcpy(image+32,load,9);
    memcpy(image+48,load,9);
    Segment segs[]={{0,10,5},{16,9,5},{32,9,4},{48,8,5}};
    assert(patch_tcb_loads(segs,4)==2);
    uint32_t displacement; memcpy(&displacement,image+5,4);
    assert(displacement==offsets[0]);
    assert(!memcmp(image+32,load,9) && !memcmp(image+48,load,9));
    assert(patch_tcb_loads(segs,4)==0);
    assert(!mprotect(image,4096,PROT_READ|PROT_EXEC));
    tcb_load=(uintptr_t (*)(void))image;
    runtime_thread_attach_main();
    uint64_t *main_tcb=check_current(123);
    pthread_t workers[3]; uint64_t *tcbs[3];
    for (size_t i=0;i<3;i++) assert(!pthread_create(&workers[i],NULL,host_worker,(void *)(i+1)));
    for (size_t i=0;i<3;i++) { assert(!pthread_join(workers[i],(void **)&tcbs[i])); assert(tcbs[i]!=main_tcb); for(size_t j=0;j<i;j++) assert(tcbs[i]!=tcbs[j]); }
    assert(tcb_load()==(uintptr_t)main_tcb && *runtime_errno()==123);
    assert(*((unsigned char *)main_tcb-8192+16)==123);
    ThreadAttr *attr=NULL; assert(!attr_init(&attr)); assert((uintptr_t)attr<(UINT64_C(1)<<40));
    GuestThread *thread=NULL; void *result=NULL;
    assert(!guest_thread_create(&thread,&attr,guest_worker,(void *)0x1234,"low TLS test"));
    assert(!thread_join(thread,&result) && result==(void *)0x1234);
    /* Inject failure after the guest record allocation, at the stack allocation. */
    fail_after=1;
    GuestThread *failed=NULL;
    assert(guest_thread_create(&failed,&attr,guest_worker,NULL,"no low stack")==ERR(12));
    assert(failed==NULL);
    fail_after=-1;
    assert(!attr_destroy(&attr) && !attr);
    puts("macOS runtime TLS: concurrent key init, loader rewrite, main/host/guest isolation, low TCB/DTV/errno/handles/stack, 8192-byte alignment passed");
    return 0;
}
