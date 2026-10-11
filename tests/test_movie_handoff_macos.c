/* BB_MOVIE_HANDOFF (src/runtime_movie_handoff.c) with the call shaped as in the game: a fake image
 * holds `mov edi, 20000; call <stub>` at the movie step's offset, the stub `jmp [slot]` and an
 * import slot holding a usleep built like the runtime's (optionally wrapped by the HLE trace),
 * and the engine objects the hook reads. Run as: untraced, traced (BB_HLE_TRACE), summary
 * (BB_HLE_TRACE_SUMMARY only). Checks:
 * - the movie step's sleep waits through the done event's wait method, in slices, until it
 *   reports done; a sleep from any other call site does not;
 * - the hook keeps the caller's errno and guest errno;
 * - loud stops (forked): a wait past the limit, a failed wait, an uninitialized event, a null
 *   renderer, an image differing from the analysed 01.09, BB_MOVIE_HANDOFF other than 0/1.
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

static uint32_t slept;
/* Shaped like runtime_kernel.c's kernel_usleep (the sleep itself is not needed here). */
static ABI int32_t test_usleep(uint32_t usec) {
    slept=usec;
    runtime_movie_handoff_after_sleep((const uintptr_t *)__builtin_frame_address(0)+1);
    return 0;
}
int runtime_kernel_is_usleep(uintptr_t function) { return function==(uintptr_t)test_usleep; }

#define IMAGE_SIZE UINT64_C(0x5544000)
#define PAGE UINT64_C(16384)
#define OTHER_SITE 0x1fce500
static unsigned char *image_base;
static unsigned char renderer[0x400], gx[0x100], config[0x100], vtable[0x40];

/* The done event's wait method (reached through a jump at image 0x207e8f0). */
static int wait_calls, timeouts_left, fail_with;
static ABI int32_t fake_wait(void *event, uint32_t microseconds) {
    assert(event==gx+GxDoneEvent && microseconds==WAIT_SLICE_US);
    ++wait_calls;
    errno=99; guest_error=77; /* the hook restores both */
    if (fail_with) return fail_with;
    if (timeouts_left) { --timeouts_left; return -3; }
    uint32_t state; memcpy(&state,gx+GxDoneEvent+EventState,4);
    state|=2; memcpy(gx+GxDoneEvent+EventState,&state,4);
    return 0;
}

static void put(uint64_t at, const void *bytes, size_t n) { memcpy(image_base+at,bytes,n); }
static void put_pointer(unsigned char *at, uintptr_t value) { memcpy(at,&value,8); }
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
    unsigned char stub[6]={0xff,0x25};
    const int32_t rel=(int32_t)(SleepGotSlot-(0x2bbfe68+6));
    memcpy(stub+2,&rel,4);
    put(0x2bbfe68,stub,6);
    put_pointer(image_base+SleepGotSlot,usleep_target);
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
static void set_state(uint32_t state) { memcpy(gx+GxDoneEvent+EventState,&state,4); }
static void call_site(uint64_t at) { ((void (*)(void))(image_base+at-4))(); }
static void movie_step(void) { call_site(SleepReturn-10); }

/* Runs `body` in a child that must stop with abort. */
static void expect_stop(const char *what, void (*body)(void)) {
    fflush(stdout);
    const pid_t pid=fork();
    assert(pid>=0);
    if (!pid) {
        freopen("/dev/null","w",stderr);
        body();
        _exit(0);
    }
    int status;
    assert(waitpid(pid,&status,0)==pid);
    assert(WIFSIGNALED(status) && WTERMSIG(status)==SIGABRT);
    printf("PASS: stops: %s\n",what);
}
static void stop_past_limit(void) { set_state(1); timeouts_left=1<<30; movie_step(); }
static void stop_failed_wait(void) { set_state(1); fail_with=-5; movie_step(); }
static void stop_uninitialized(void) { set_state(0); movie_step(); }
static void stop_null_renderer(void) { put_pointer(image_base+RendererGlobal,0); movie_step(); }
static void stop_mismatch(void) {
    setenv("BB_MOVIE_HANDOFF","1",1);
    runtime_movie_handoff_configure(image_base,IMAGE_SIZE);
}
static void stop_bad_setting(void) {
    setenv("BB_MOVIE_HANDOFF","2",1);
    runtime_movie_handoff_configure(image_base,IMAGE_SIZE);
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
    set_state(1);
    movie_step();
    assert(slept==20000 && wait_calls==0);

    runtime_movie_handoff_enable_for_test(image_base,UINT64_C(200000000));
    set_state(1); /* GX still building */
    timeouts_left=2;
    errno=17; guest_error=23;
    movie_step();
    assert(slept==20000 && wait_calls==3 && waited==1);
    assert(errno==17 && guest_error==23);
    printf("PASS: %s: the movie step's sleep waits for GX's done event (2 slices timed out, "
           "then done); errno and guest errno kept\n",mode);

    wait_calls=0;
    set_state(3); /* GX done within the sleep: the wait returns at once */
    movie_step();
    assert(wait_calls==1 && waited==1 && steps==2);
    printf("PASS: %s: GX already done: one wait call, nothing waited\n",mode);

    wait_calls=0;
    set_state(1);
    call_site(OTHER_SITE);
    assert(slept==1 && wait_calls==0 && steps==2);
    printf("PASS: %s: a sleep from another call site does not wait\n",mode);

    expect_stop("GX not done within the wait limit",stop_past_limit);
    expect_stop("the done event's wait fails",stop_failed_wait);
    expect_stop("the done event is not initialized",stop_uninitialized);
    expect_stop("the renderer global is null",stop_null_renderer);
    assert(runtime_movie_handoff_verify(image_base,IMAGE_SIZE));
    assert(runtime_movie_handoff_verify(image_base,0x1000));
    expect_stop("BB_MOVIE_HANDOFF=1 over an image that is not the analysed 01.09",stop_mismatch);
    expect_stop("BB_MOVIE_HANDOFF=2",stop_bad_setting);
    return 0;
}
