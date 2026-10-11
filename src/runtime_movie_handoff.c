/* bbport (macOS, opt-in BB_MOVIE_HANDOFF=1): the movie step's handoff to GXRenderThread.
 *
 * Bloodborne 01.09's movie step (image 0x1fce3b0, main thread) sleeps 20 ms (the usleep call at
 * 0x1fce3fa), then draws the movie into the renderer's context [[[0x55406c8]+0x10]+8], the
 * command stream GXRenderThread builds its frames in. Nothing between orders the two: the step
 * relies on GX having finished its frame within the 20 ms, as it does on the PS4. Translated by
 * Rosetta GX often has not, both threads advance the one stream (CMD-INTERLEAVE) and the GPU
 * reads torn packets.
 *
 * Here, after that one sleep, the step also waits for GX's own frame-done event: G+0x68, G =
 * [[0x55406c8]+0x348]. GX sets it after each frame job (0x21b6a68), the kick resets it
 * (0x21b6aa0). The wait goes through the engine's wait method (event vtable +0x18 = 0x207e8f0:
 * the event's mutex and condition, which returns at once when it is set), in slices, and stops
 * loudly after 5 s. Where GX finished within the 20 ms (the PS4's timing) the event is already
 * set and nothing changes.
 *
 * Not proven (an experiment, hence opt-in): that only the main thread kicks GX, so that no kick
 * comes between this wait and the draw. The kick's mode byte ([[G+0x88]+0x72]) is logged.
 *
 * Every code range this relies on is checked against the analysed 01.09 image at load, after
 * relocation and patches; a mismatch stops. The call site is matched by its exact guest return
 * address, recovered from the HLE trace's record when tracing replaced the return slot. */
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
    RendererGlobal = 0x55406c8,    /* R */
    GxInRenderer = 0x348,          /* G = [R+0x348] (the kick wrapper 0x2199be0) */
    GxDoneEvent = 0x68,
    GxConfig = 0x88,
    ConfigKickMode = 0x72,         /* [[G+0x88]+0x72]: the kick's mode */
    EventState = 0x18,             /* bit 0 initialized, bit 1 signaled */
    EventWaitSlot = 0x18,          /* in the event's vtable */
    EventWaitMethod = 0x207e8f0,   /* wait(event, microseconds or -1): 0 signaled, -3 timeout */
};
#define WAIT_SLICE_US 100000u
#define WAIT_LIMIT_NS UINT64_C(5000000000)
#define REPORT_EVERY 600

typedef struct { const char *what; uint64_t offset, size, fnv; } Range;
/* FNV-1a 64 of each range in the analysed 01.09 image (after relocation; none is relocated). */
static const Range handoff_ranges[]={
    {"movie step: 20 ms sleep, renderer context, movie draw", 0x1fce3f5, 0x42, 0x041203f2f35f4105ull},
    {"sleep import stub", 0x2bbfe68, 0x6, 0x12796d0702d08f3full},
    {"GX loop: done event at G+0x68", 0x21b694e, 0x11, 0x9c3b71f8111d7506ull},
    {"GX loop: frame job, done set, kick wait", 0x21b6a49, 0x33, 0xed506fdd33ff3061ull},
    {"kick (0x21b6aa0)", 0x21b6aa0, 0x7f, 0x3c9a3772b8dda1d3ull},
    {"kick wrapper: G = [R+0x348]", 0x2199bf1, 0x2f, 0xc14097d8d785f5b6ull},
    {"event set: bit 1 = signaled", 0x207e855, 0x13, 0x41256cbdf9028df3ull},
    {"event wait method (0x207e8f0)", 0x207e8f0, 0xfb, 0x90dc716c25e81e7cull},
};

typedef int32_t (ABI *EventWait)(void *event, uint32_t microseconds);

static const unsigned char *handoff_image;
static int handoff_on;
static int last_mode=-1;
static uint64_t steps, waited, wait_ns, wait_max_ns;

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

/* NULL when every range matches, else the first that does not. */
const char *runtime_movie_handoff_verify(const unsigned char *base, uint64_t size) {
    for (size_t i=0;i<sizeof(handoff_ranges)/sizeof(handoff_ranges[0]);++i) {
        const Range *r=&handoff_ranges[i];
        if (r->offset+r->size>size || handoff_fnv(base+r->offset,r->size)!=r->fnv) return r->what;
    }
    return NULL;
}

/* Test controls: enable over `base` without the load-time checks; a shorter wait limit. */
static uint64_t wait_limit_ns=WAIT_LIMIT_NS;
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
    printf("Movie handoff: on (after the movie step's 20 ms sleep, wait for GXRenderThread's "
           "frame-done event)\n");
    fflush(stdout);
}

static uintptr_t handoff_pointer(uintptr_t at, const char *what) {
    uintptr_t value;
    memcpy(&value,(const void *)at,sizeof(value));
    if (!value) handoff_stop(what,at,0);
    return value;
}

void runtime_movie_handoff_after_sleep(const uintptr_t *return_slot) {
    if (!handoff_on) return;
    if (runtime_hle_trace_original_return(return_slot)!=(uintptr_t)handoff_image+SleepReturn) return;
    const int host_errno=errno;
    int32_t *guest_errno=runtime_errno();
    const int32_t guest_saved=*guest_errno;

    const uintptr_t renderer=handoff_pointer((uintptr_t)handoff_image+RendererGlobal,"renderer [0x55406c8] is null");
    const uintptr_t gx=handoff_pointer(renderer+GxInRenderer,"GX object [R+0x348] is null");
    void *done=(void *)(gx+GxDoneEvent);
    uint32_t state;
    memcpy(&state,(const char *)done+EventState,sizeof(state));
    if (!(state&1)) handoff_stop("GX's done event is not initialized",gx,state);
    const uintptr_t vtable=handoff_pointer((uintptr_t)done,"GX's done event has no vtable");
    const uintptr_t wait=handoff_pointer(vtable+EventWaitSlot,"GX's done event has no wait method");
    if (wait!=(uintptr_t)handoff_image+EventWaitMethod) handoff_stop("GX's done event wait is not 0x207e8f0",wait,vtable);
    const unsigned char mode=*(const unsigned char *)(handoff_pointer(gx+GxConfig,"GX config is null")+ConfigKickMode);
    if (mode!=last_mode) {
        char name[64]="";
        pthread_getname_np(pthread_self(),name,sizeof(name));
        printf("Movie handoff: kick mode byte %u (was %d), movie step on thread \"%s\", G %#" PRIxPTR "\n",
               mode,last_mode,name,gx);
        fflush(stdout);
        last_mode=mode;
    }

    ++steps;
    /* Statistics only: the wait below decides under the event's mutex. */
    const int was_done=__atomic_load_n((const uint32_t *)((const char *)done+EventState),
                                       __ATOMIC_RELAXED)&2;
    const uint64_t start=handoff_now_ns();
    for (;;) {
        const int32_t result=((EventWait)wait)(done,WAIT_SLICE_US);
        if (!result) break;
        if (result!=-3) handoff_stop("GX's done event wait failed",(uint64_t)(uint32_t)result,gx);
        if (handoff_now_ns()-start>wait_limit_ns) {
            uint32_t now;
            memcpy(&now,(const char *)done+EventState,sizeof(now));
            handoff_stop("GX's frame is not done within the wait limit (deadlock?): event state, G",now,gx);
        }
    }
    if (!was_done) {
        const uint64_t ns=handoff_now_ns()-start;
        ++waited; wait_ns+=ns;
        if (ns>wait_max_ns) wait_max_ns=ns;
    }
    if (steps%REPORT_EVERY==0) {
        printf("Movie handoff: %" PRIu64 " movie steps, %" PRIu64 " waited for GX (%.2f ms mean, "
               "%.2f ms max)\n",steps,waited,waited ? wait_ns/1e6/waited : 0.0,wait_max_ns/1e6);
        fflush(stdout);
    }
    *guest_errno=guest_saved;
    errno=host_errno;
}
#else
void runtime_movie_handoff_configure(const unsigned char *base, uint64_t size) { (void)base; (void)size; }
void runtime_movie_handoff_after_sleep(const uintptr_t *return_slot) { (void)return_slot; }
#endif
