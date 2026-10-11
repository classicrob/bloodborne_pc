/* BB_MOVIE_HANDOFF (src/runtime_movie_handoff.c) with the call shaped as in the game: a fake image
 * holds `mov edi, 20000; call <stub>` at the movie step's offset, the stubs `jmp [slot]` for the
 * sleep and for the engine's mutex lock/unlock, import slots, a jump to the event's wait method,
 * and the engine objects the hook reads. Run as: untraced, traced (BB_HLE_TRACE), summary
 * (BB_HLE_TRACE_SUMMARY only).
 * Limit: the sleep in the slot is test_usleep, built like runtime_kernel.c's kernel_usleep (it
 * reports its return slot after the sleep), not runtime_kernel.c itself.
 * The fake wait method follows a script per call: T timeout (-3), S a wake (0) with done still
 * clear, D GX sets done (under the event's mutex) and the wait returns 0, L the same after the
 * wait limit, F a failure.
 * Checks:
 * - the movie step's sleep waits until done is seen under the event's mutex: after timeouts,
 *   after wakes that are not done; already done means no wait; another call site never waits;
 * - errno and guest errno kept; two threads at once (per-thread statistics);
 * - the trace's original return: a stale record for the same slot does not apply to a slot that
 *   does not hold the bridge;
 * - loud stops (forked, each matched by its message): not done by the deadline (timeouts; wakes
 *   that are not done; done only after the deadline), a failed wait, an uninitialized event, a null renderer, a bridge without
 *   its record, an image differing from the analysed 01.09, an image too small for the slots,
 *   BB_MOVIE_HANDOFF other than 0/1.
 * The analysed game code is not part of the repository, so the image check is exercised with a
 * mismatching image only; a game run prints "Movie handoff: on" when it matches. */
#define _GNU_SOURCE
#define BB_HLE_TRACE_TEST
#include "../src/runtime_hle_trace.c"
#include "../src/runtime_movie_handoff.c"
#include <assert.h>
#include <signal.h>
#include <sys/wait.h>

static _Thread_local int32_t guest_error;
int32_t *runtime_errno(void) { return &guest_error; }
void *runtime_low_map_named(size_t n,int prot,const char *owner) {
    (void)owner;void *p=mmap(NULL,n,prot,MAP_PRIVATE|MAP_ANON,-1,0);
    return p==MAP_FAILED ? NULL : p;
}
void hle_trace_publication_hook(void) {}

static _Thread_local uint32_t slept;
static ABI int32_t test_usleep(uint32_t usec) {
    slept=usec;
    runtime_movie_handoff_after_sleep((const uintptr_t *)__builtin_frame_address(0)+1);
    return 0;
}
int runtime_kernel_is_usleep(uintptr_t function) { return function==(uintptr_t)test_usleep; }

#define IMAGE_SIZE UINT64_C(0x5544000)
#define PAGE UINT64_C(16384)
#define OTHER_SITE 0x1fce500
#define LOCK_SLOT 0x53e53b0
#define UNLOCK_SLOT 0x53e53b8
static unsigned char *image_base;
static unsigned char renderer[0x400], gx[0x100], config[0x100], vtable[0x40];
static pthread_mutex_t event_mutex=PTHREAD_MUTEX_INITIALIZER;
static _Thread_local int holding;
static _Thread_local atomic_int *entered; /* a concurrent worker's: set once it waits */
static atomic_int locks, wait_calls, script_at;
static const char *script="";

static void set_state(uint32_t state) {
    pthread_mutex_lock(&event_mutex);
    memcpy(gx+GxDoneEvent+EventState,&state,4);
    pthread_mutex_unlock(&event_mutex);
}
static ABI int32_t fake_lock(void *mutex) {
    assert(mutex==gx+GxDoneEvent+EventMutex && !holding);
    pthread_mutex_lock(&event_mutex); holding=1; ++locks;
    return 0;
}
static ABI int32_t fake_unlock(void *mutex) {
    assert(mutex==gx+GxDoneEvent+EventMutex && holding);
    holding=0; pthread_mutex_unlock(&event_mutex);
    return 0;
}
/* The engine's wait method: called without the mutex held; waits once. */
static ABI int32_t fake_wait(void *event, uint32_t microseconds) {
    assert(event==gx+GxDoneEvent && microseconds==WAIT_SLICE_US && !holding);
    ++wait_calls;
    if (entered) atomic_store(entered,1);
    errno=99; guest_error=77; /* the hook restores both */
    const int at=atomic_fetch_add(&script_at,1);
    const size_t length=strlen(script);
    const char step=(size_t)at<length ? script[at] : script[length-1];
    switch (step) {
    case 'T': usleep(1000); return -3;
    case 'S': return 0;
    case 'L': usleep(400000); /* past the test's 300 ms limit, then done */ /* fall through */
    case 'D': {
        pthread_mutex_lock(&event_mutex);
        uint32_t state; memcpy(&state,gx+GxDoneEvent+EventState,4);
        state|=2; memcpy(gx+GxDoneEvent+EventState,&state,4);
        pthread_mutex_unlock(&event_mutex);
        return 0;
    }
    default: return -5;
    }
}

static void put(uint64_t at, const void *bytes, size_t n) { memcpy(image_base+at,bytes,n); }
static void put_pointer(unsigned char *at, uintptr_t value) { memcpy(at,&value,8); }
static void put_stub(uint64_t at, uint64_t slot, uintptr_t target) {
    unsigned char stub[6]={0xff,0x25};
    const int32_t rel=(int32_t)(slot-(at+6));
    memcpy(stub+2,&rel,4);
    put(at,stub,6);
    put_pointer(image_base+slot,target);
}
/* At `at`-4: sub rsp, 8 (the callee sees an aligned stack); at `at`: mov edi, usec; call stub;
 * then add rsp, 8; ret. Entered at `at`-4. */
static void put_call_site(uint64_t at, uint32_t usec) {
    unsigned char code[19]={0x48,0x83,0xec,0x08, 0xbf,0,0,0,0, 0xe8,0,0,0,0, 0x48,0x83,0xc4,0x08, 0xc3};
    memcpy(code+5,&usec,4);
    const int32_t rel=(int32_t)(0x2bbfe68-(at+10));
    memcpy(code+10,&rel,4);
    put(at-4,code,sizeof(code));
}
static void build_image(uintptr_t usleep_target) {
    image_base=mmap(NULL,IMAGE_SIZE,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
    assert(image_base!=MAP_FAILED);
    put_call_site(SleepReturn-10,20000);
    put_call_site(OTHER_SITE,1);
    put_stub(0x2bbfe68,SleepGotSlot,usleep_target);
    put_stub(MutexLockStub,LOCK_SLOT,(uintptr_t)fake_lock);
    put_stub(MutexUnlockStub,UNLOCK_SLOT,(uintptr_t)fake_unlock);
    unsigned char jump[12]={0x48,0xb8,0,0,0,0,0,0,0,0,0xff,0xe0}; /* movabs rax; jmp rax */
    const uintptr_t target=(uintptr_t)fake_wait;
    memcpy(jump+2,&target,8);
    put(EventWaitMethod,jump,sizeof(jump));
    put_pointer(image_base+RendererGlobal,(uintptr_t)renderer);
    put_pointer(renderer+GxInRenderer,(uintptr_t)gx);
    put_pointer(gx+GxDoneEvent,(uintptr_t)vtable);
    put_pointer(vtable+EventWaitSlot,(uintptr_t)image_base+EventWaitMethod);
    put_pointer(gx+GxConfig,(uintptr_t)config);
    config[ConfigKickMode]=1;
    const uint64_t code_pages[]={0x1fcc000,0x207c000,0x2bbc000};
    for (size_t i=0;i<3;++i)
        assert(!mprotect(image_base+code_pages[i],PAGE,PROT_READ|PROT_EXEC));
}
static void call_site(uint64_t at) { ((void (*)(void))(image_base+at-4))(); }
static void movie_step(void) { call_site(SleepReturn-10); }
static void run(const char *steps_script, uint32_t state) {
    script=steps_script;
    atomic_store(&script_at,0); atomic_store(&wait_calls,0); atomic_store(&locks,0);
    set_state(state);
    movie_step();
}

/* Runs `body` in a child that must stop with abort and print `message`. */
static void expect_stop(const char *what, const char *message, void (*body)(void)) {
    fflush(stdout);
    int out[2];
    assert(!pipe(out));
    const pid_t pid=fork();
    assert(pid>=0);
    if (!pid) {
        dup2(out[1],2); close(out[0]);
        body();
        _exit(0);
    }
    close(out[1]);
    char text[4096]; size_t n=0; ssize_t got;
    while (n<sizeof(text)-1 && (got=read(out[0],text+n,sizeof(text)-1-n))>0) n+=(size_t)got;
    text[n]=0; close(out[0]);
    int status;
    assert(waitpid(pid,&status,0)==pid);
    if (!(WIFSIGNALED(status) && WTERMSIG(status)==SIGABRT && strstr(text,message))) {
        fprintf(stderr,"FAIL: %s: expected an abort with \"%s\", got status %d, stderr:\n%s\n",
                what,message,status,text);
        abort();
    }
    printf("PASS: stops: %s\n",what);
}
static void stop_past_limit(void) { run("T",1); }
static void stop_wakes_not_done(void) { run("S",1); }
static void stop_done_late(void) { run("L",1); }
static void stop_failed_wait(void) { run("F",1); }
static void stop_uninitialized(void) { run("D",0); }
static void stop_null_renderer(void) { put_pointer(image_base+RendererGlobal,0); run("D",1); }
static void stop_bridge_without_record(void) {
    uintptr_t slot=(uintptr_t)runtime_hle_trace_return_bridge;
    depth=0;
    (void)runtime_hle_trace_original_return(&slot);
}
static void stop_mismatch(void) {
    setenv("BB_MOVIE_HANDOFF","1",1);
    runtime_movie_handoff_configure(image_base,IMAGE_SIZE);
}
static void stop_too_small(void) {
    setenv("BB_MOVIE_HANDOFF","1",1);
    runtime_movie_handoff_configure(image_base,0x3000000);
}
static void stop_bad_setting(void) {
    setenv("BB_MOVIE_HANDOFF","2",1);
    runtime_movie_handoff_configure(image_base,IMAGE_SIZE);
}

static atomic_int worker_entered[2];
static void *concurrent_step(void *worker) {
    entered=&worker_entered[(intptr_t)worker];
    movie_step();
    assert(slept==20000 && steps==1 && waited==1);
    return NULL;
}

int main(int argc, char **argv) {
    const char *mode=argc>1 ? argv[1] : "untraced";
    if (!strcmp(mode,"traced")) {
        setenv("BB_HLE_TRACE","sceKernelUsleep",1);
    } else if (!strcmp(mode,"summary")) {
        setenv("BB_HLE_TRACE_SUMMARY","1",1);
        setenv("BB_HLE_TRACE_SUMMARY_FILTER","sceKernelUsleep",1);
    } else {
        assert(!strcmp(mode,"untraced"));
    }
    const uintptr_t target=runtime_hle_trace_wrap("sceKernelUsleep","sceKernelUsleep",
                                                 (uintptr_t)test_usleep);
    assert(!strcmp(mode,"untraced") ? target==(uintptr_t)test_usleep
                                    : target!=(uintptr_t)test_usleep);
    assert(runtime_hle_trace_unwrap(target)==(uintptr_t)test_usleep);
    build_image(target);

    /* Not enabled (BB_MOVIE_HANDOFF unset): the movie step only sleeps. */
    runtime_movie_handoff_configure(image_base,IMAGE_SIZE);
    run("D",1);
    assert(slept==20000 && wait_calls==0 && locks==0);

    runtime_movie_handoff_enable_for_test(image_base,UINT64_C(300000000));
    errno=17; guest_error=23;
    run("TTD",1);
    assert(slept==20000 && wait_calls==3 && locks==4 && waited==1);
    assert(errno==17 && guest_error==23);
    printf("PASS: %s: waits through 2 timeouts until done is seen under the mutex; errno and "
           "guest errno kept\n",mode);

    run("SSD",1);
    assert(wait_calls==3 && locks==4 && waited==2);
    printf("PASS: %s: 2 wakes with done still clear do not end the wait\n",mode);

    run("D",3);
    assert(wait_calls==0 && locks==1 && waited==2 && steps==3);
    printf("PASS: %s: GX already done (seen under the mutex): no wait\n",mode);

    script="D"; atomic_store(&wait_calls,0); atomic_store(&locks,0); set_state(1);
    call_site(OTHER_SITE);
    assert(slept==1 && wait_calls==0 && locks==0 && steps==3);
    printf("PASS: %s: a sleep from another call site does not wait\n",mode);

    /* Two movie steps at once: both wait until done; statistics are per thread. */
    script="T"; atomic_store(&script_at,0); set_state(1);
    pthread_t a,b;
    assert(!pthread_create(&a,NULL,concurrent_step,(void *)0));
    assert(!pthread_create(&b,NULL,concurrent_step,(void *)1));
    while (!atomic_load(&worker_entered[0]) || !atomic_load(&worker_entered[1])) usleep(1000);
    set_state(3); /* only once both are inside the wait */
    assert(!pthread_join(a,NULL) && !pthread_join(b,NULL));
    printf("PASS: %s: two movie steps at once both wait for done\n",mode);

    /* A stale record naming this slot, the slot not holding the bridge: the slot's own value. */
    uintptr_t slot=0x1234;
    const unsigned saved_depth=depth;
    calls[depth]=(TraceCall){.slot=(uintptr_t)&slot,.ret=0x5678};
    ++depth;
    assert(runtime_hle_trace_original_return(&slot)==0x1234);
    depth=saved_depth;
    printf("PASS: %s: a stale trace record for the same slot is ignored\n",mode);

    expect_stop("not done by the deadline (timeouts)","not done within the wait limit",
                stop_past_limit);
    expect_stop("not done by the deadline (wakes, done still clear)",
                "not done within the wait limit",stop_wakes_not_done);
    expect_stop("done only after the deadline","not done within the wait limit",stop_done_late);
    expect_stop("the done event's wait fails","done event wait failed",stop_failed_wait);
    expect_stop("the done event is not initialized","not initialized",stop_uninitialized);
    expect_stop("the renderer global is null","renderer [0x55406c8] is null",stop_null_renderer);
    expect_stop("a return bridge without its call record","without its call record",
                stop_bridge_without_record);
    assert(runtime_movie_handoff_verify(image_base,IMAGE_SIZE));
    expect_stop("BB_MOVIE_HANDOFF=1 over an image that is not the analysed 01.09",
                "differs from the analysed",stop_mismatch);
    expect_stop("BB_MOVIE_HANDOFF=1 over an image too small for the slots read","image size",
                stop_too_small);
    expect_stop("BB_MOVIE_HANDOFF=2","must be 0 or 1",stop_bad_setting);
    return 0;
}
