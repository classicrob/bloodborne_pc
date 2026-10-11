/* bbport (macOS, opt-in BB_MOVIE_HANDOFF=1): an ordering experiment for the movie step's handoff
 * to GXRenderThread.
 *
 * Bloodborne 01.09's movie step (image 0x1fce3b0, main thread) sleeps 20 ms (the usleep call at
 * 0x1fce3fa), then draws the movie into the renderer's context [[[0x55406c8]+0x10]+8], the
 * command stream GXRenderThread builds its frames in. Nothing found between the two orders them:
 * our hypothesis is that the step relies on GX finishing its frame within the 20 ms (on the PS4
 * this is inferred, not measured). Translated by Rosetta, both threads are seen advancing the one
 * stream (CMD-INTERLEAVE) and the GPU then reads torn packets.
 *
 * Here, after that one sleep, the step also waits for GX's own frame-done event: G+0x68, G =
 * [[0x55406c8]+0x348]. GX sets it after each frame job (0x21b6a68), the kick resets it
 * (0x21b6aa0). Done is read only under the event's mutex (event+8, through the engine's own
 * mutex imports); while it is clear the step waits on the event's condition through the engine's
 * wait method (event vtable +0x18 = 0x207e8f0), in slices. That method waits once and returns 0
 * for any wake, so a 0 is never taken as done: done is checked again under the mutex. A 5 s
 * deadline is checked after every check of done and stops loudly; it bounds repeated waits that
 * return, not a lock or wait that never returns.
 *
 * If only the main thread kicks GX (not established; the qualification run records the kick's
 * caller threads), no kick can come between this wait and the draw, and the draw then follows
 * GX's frame. Where GX already finished within the sleep, nothing changes.
 *
 * Every code range this relies on is checked against the analysed 01.09 image at load, after
 * relocation and patches; a mismatch stops. The call site is matched by its exact guest return
 * address, recovered from the HLE trace's record only when tracing replaced the return slot. */
#include "runtime.h"
#include "runtime_hle_trace.h"
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef __APPLE__
enum {
    SleepReturn = 0x1fce3ff,       /* after the movie step's 20 ms usleep call */
    SleepGotSlot = 0x53e5368,      /* the usleep stub's import slot */
    MutexLockStub = 0x2bbfef8,     /* the engine's mutex lock import (the wait method's) */
    MutexUnlockStub = 0x2bbff08,
    RendererGlobal = 0x55406c8,    /* R */
    GxInRenderer = 0x348,          /* G = [R+0x348] (the kick wrapper 0x2199be0) */
    GxDoneEvent = 0x68,
    GxConfig = 0x88,
    ConfigKickMode = 0x72,         /* [[G+0x88]+0x72]: the kick's mode */
    EventMutex = 0x8,
    EventState = 0x18,             /* bit 0 initialized, bit 1 signaled; written under the mutex */
    EventWaitSlot = 0x18,          /* in the event's vtable */
    EventWaitMethod = 0x207e8f0,   /* wait(event, microseconds or -1): 0 any wake, -3 timeout */
};
#define WAIT_SLICE_US 100000u
#define WAIT_LIMIT_NS UINT64_C(5000000000)
#define REPORT_EVERY 600

typedef struct { const char *what; uint64_t offset, size, fnv; } Range;
/* FNV-1a 64 of each range in the analysed 01.09 image (after relocation; none is relocated). */
static const Range handoff_ranges[]={
    {"movie step: 20 ms sleep, renderer context, movie draw", 0x1fce3f5, 0x42, 0x041203f2f35f4105ull},
    {"sleep import stub", 0x2bbfe68, 0x6, 0x12796d0702d08f3full},
    {"mutex lock import stub", 0x2bbfef8, 0x6, 0xbff78ed76d75a277ull},
    {"mutex unlock import stub", 0x2bbff08, 0x6, 0x25fe8fa2909b51efull},
    {"GX loop: done event at G+0x68", 0x21b694e, 0x11, 0x9c3b71f8111d7506ull},
    {"GX loop: frame job, done set, kick wait", 0x21b6a49, 0x33, 0xed506fdd33ff3061ull},
    {"kick (0x21b6aa0), both modes", 0x21b6aa0, 0x131, 0xb3d30b1c22c4e2d8ull},
    {"kick wrapper: G = [R+0x348]", 0x2199bf1, 0x2f, 0xc14097d8d785f5b6ull},
    {"event set: bit 1 = signaled", 0x207e855, 0x13, 0x41256cbdf9028df3ull},
    {"event wait method (0x207e8f0) through unlock and return", 0x207e8f0, 0x149, 0xe13c769c0a82343cull},
    {"condition wait helper (0x2083e80): microseconds, -1, timeout -3", 0x2083e80, 0xd0, 0x79903a6a15eff0b7ull},
};

typedef int32_t (ABI *EventWait)(void *event, uint32_t microseconds);
typedef int32_t (ABI *MutexCall)(void *mutex);

static const unsigned char *handoff_image;
static int handoff_on;
static uint64_t wait_limit_ns=WAIT_LIMIT_NS;
/* Per thread: the movie step is expected on main only, which is not established. */
static _Thread_local int last_mode=-1;
static _Thread_local uint64_t steps, waited, wait_ns, wait_max_ns;

static void handoff_stop(const char *why, uint64_t a, uint64_t b) {
    fprintf(stderr,"STOP: BB_MOVIE_HANDOFF %s (%#" PRIx64 ", %#" PRIx64 ")\n",why,a,b);
    abort();
}
static uint64_t handoff_now_ns(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000000+(uint64_t)t.tv_nsec;
}
static uint64_t handoff_fnv(const unsigned char *p, uint64_t n) {
    uint64_t h=0xcbf29ce484222325ull;
    for (uint64_t i=0;i<n;++i) h=(h^p[i])*0x100000001b3ull;
    return h;
}

/* NULL when the image holds every range and slot read at load and every range matches, else what
 * does not. */
const char *runtime_movie_handoff_verify(const unsigned char *base, uint64_t size) {
    if (size<RendererGlobal+8 || size<SleepGotSlot+8) return "image size (the slots read)";
    for (size_t i=0;i<sizeof(handoff_ranges)/sizeof(handoff_ranges[0]);++i) {
        const Range *r=&handoff_ranges[i];
        if (r->offset+r->size>size || handoff_fnv(base+r->offset,r->size)!=r->fnv) return r->what;
    }
    return NULL;
}

/* Test controls: enable over `base` without the load-time checks; a shorter wait limit. */
void runtime_movie_handoff_enable_for_test(const unsigned char *base, uint64_t limit_ns) {
    handoff_image=base; handoff_on=1; wait_limit_ns=limit_ns;
}

void runtime_movie_handoff_configure(const unsigned char *base, uint64_t size) {
    const char *setting=getenv("BB_MOVIE_HANDOFF");
    if (!setting || !*setting || !strcmp(setting,"0")) return;
    if (strcmp(setting,"1")) handoff_stop("must be 0 or 1",0,0);
    const char *mismatch=runtime_movie_handoff_verify(base,size);
    if (mismatch) {
        fprintf(stderr,"STOP: BB_MOVIE_HANDOFF=1, but the image differs from the analysed "
                "Bloodborne 01.09 at: %s\n",mismatch);
        abort();
    }
    uintptr_t slot;
    memcpy(&slot,base+SleepGotSlot,sizeof(slot));
    if (!runtime_kernel_is_usleep(runtime_hle_trace_unwrap(slot)))
        handoff_stop("the movie step's sleep import is not the runtime's usleep",slot,0);
    handoff_image=base; handoff_on=1;
    printf("Movie handoff: on (experiment: after the movie step's 20 ms sleep, wait for "
           "GXRenderThread's frame-done event)\n");
    fflush(stdout);
}

static uintptr_t handoff_pointer(uintptr_t at, const char *what) {
    uintptr_t value;
    memcpy(&value,(const void *)at,sizeof(value));
    if (!value) handoff_stop(what,at,0);
    return value;
}

/* The event's state, read under the event's mutex as the engine reads it (bit 1: done). */
static uint32_t state_locked(void *done, uintptr_t gx) {
    void *mutex=(char *)done+EventMutex;
    const int32_t locked=((MutexCall)(handoff_image+MutexLockStub))(mutex);
    if (locked) handoff_stop("locking GX's done event failed",(uint64_t)(uint32_t)locked,gx);
    uint32_t state;
    memcpy(&state,(const char *)done+EventState,sizeof(state));
    const int32_t unlocked=((MutexCall)(handoff_image+MutexUnlockStub))(mutex);
    if (unlocked) handoff_stop("unlocking GX's done event failed",(uint64_t)(uint32_t)unlocked,gx);
    return state;
}

void runtime_movie_handoff_after_sleep(const uintptr_t *return_slot) {
    if (!handoff_on) return;
    if (runtime_hle_trace_original_return(return_slot)!=(uintptr_t)handoff_image+SleepReturn) return;
    const int host_errno=errno;
    int32_t *guest_errno=runtime_errno();
    const int32_t guest_saved=*guest_errno;

    const uintptr_t renderer=handoff_pointer((uintptr_t)handoff_image+RendererGlobal,
                                             "renderer [0x55406c8] is null");
    const uintptr_t gx=handoff_pointer(renderer+GxInRenderer,"GX object [R+0x348] is null");
    void *done=(void *)(gx+GxDoneEvent);
    uint32_t state;
    memcpy(&state,(const char *)done+EventState,sizeof(state));
    if (!(state&1)) handoff_stop("GX's done event is not initialized",gx,state);
    const uintptr_t vtable=handoff_pointer((uintptr_t)done,"GX's done event has no vtable");
    const uintptr_t wait=handoff_pointer(vtable+EventWaitSlot,"GX's done event has no wait method");
    if (wait!=(uintptr_t)handoff_image+EventWaitMethod)
        handoff_stop("GX's done event wait is not 0x207e8f0",wait,vtable);
    const uintptr_t config=handoff_pointer(gx+GxConfig,"GX config is null");
    const unsigned char mode=*(const unsigned char *)(config+ConfigKickMode);
    if (mode!=last_mode) {
        char name[64]="";
        pthread_getname_np(pthread_self(),name,sizeof(name));
        printf("Movie handoff: kick mode byte %u (was %d), movie step on thread \"%s\", G %#" PRIxPTR "\n",
               mode,last_mode,name,gx);
        fflush(stdout);
        last_mode=mode;
    }

    ++steps;
    /* The limit is checked after every check of done, so done seen past it still stops. It is a
     * deadline between engine calls, not a watchdog: a lock or wait that never returns is not
     * bounded by it. */
    const uint64_t start=handoff_now_ns();
    int was_done=1;
    for (;;) {
        const uint32_t now_state=state_locked(done,gx);
        if (handoff_now_ns()-start>wait_limit_ns)
            handoff_stop("GX's frame was not done within the wait limit (deadlock?): G, state",gx,
                         now_state);
        if (now_state&2) break;
        was_done=0;
        const int32_t result=((EventWait)wait)(done,WAIT_SLICE_US);
        if (result && result!=-3) handoff_stop("GX's done event wait failed",(uint64_t)(uint32_t)result,gx);
    }
    if (!was_done) {
        const uint64_t ns=handoff_now_ns()-start;
        ++waited; wait_ns+=ns;
        if (ns>wait_max_ns) wait_max_ns=ns;
    }
    if (steps%REPORT_EVERY==0) {
        char name[64]="";
        pthread_getname_np(pthread_self(),name,sizeof(name));
        printf("Movie handoff (thread \"%s\"): %" PRIu64 " movie steps, %" PRIu64 " waited for GX "
               "(%.2f ms mean, %.2f ms max)\n",name,steps,waited,waited ? wait_ns/1e6/waited : 0.0,
               wait_max_ns/1e6);
        fflush(stdout);
    }
    *guest_errno=guest_saved;
    errno=host_errno;
}
#else
void runtime_movie_handoff_configure(const unsigned char *base, uint64_t size) { (void)base; (void)size; }
void runtime_movie_handoff_after_sleep(const uintptr_t *return_slot) { (void)return_slot; }
#endif
