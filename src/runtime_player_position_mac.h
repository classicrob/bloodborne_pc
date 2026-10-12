#ifndef BB_RUNTIME_PLAYER_POSITION_MAC_H
#define BB_RUNTIME_PLAYER_POSITION_MAC_H
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef bool (*PlayerRead)(uintptr_t, void *, size_t, void *);
typedef struct {
    uintptr_t world, player, modules, physics;
    uint32_t map;
    float rotation[4], position[4];
} PlayerObservation;
typedef enum { PLAYER_OK, PLAYER_ABSENT, PLAYER_UNREADABLE, PLAYER_CHANGED,
               PLAYER_NONFINITE, PLAYER_NO_MAP, PLAYER_STARTING } PlayerState;
static const char *player_reason(PlayerState s) {
    static const char *names[]={"ok","absent-player-chain","unreadable-player-chain",
        "changing-player-state","non-finite-transform","map-unavailable","waiting-for-pad-read"};
    return names[s];
}
static bool player_field(PlayerRead read, void *ctx, uintptr_t base, size_t offset,
                         void *out, size_t size) {
    return base && offset<=UINTPTR_MAX-base && size<=UINTPTR_MAX-(base+offset) &&
           read(base+offset,out,size,ctx);
}
static PlayerState player_walk(PlayerRead read, void *ctx, uintptr_t image, PlayerObservation *o) {
    memset(o,0,sizeof(*o));
    if (!player_field(read,ctx,image,0x553e878,&o->world,8)) return PLAYER_UNREADABLE;
    if (!o->world) return PLAYER_ABSENT;
    if (!player_field(read,ctx,o->world,0x60,&o->player,8)) return PLAYER_UNREADABLE;
    if (!o->player) return PLAYER_ABSENT;
    if (!player_field(read,ctx,o->player,0x3b0,&o->modules,8)) return PLAYER_UNREADABLE;
    if (!o->modules) return PLAYER_ABSENT;
    if (!player_field(read,ctx,o->modules,0x68,&o->physics,8)) return PLAYER_UNREADABLE;
    if (!o->physics) return PLAYER_ABSENT;
    if (!player_field(read,ctx,o->player,0x3f8,&o->map,4) ||
        !player_field(read,ctx,o->physics,0x1d0,&o->rotation,32)) return PLAYER_UNREADABLE;
    if (o->map==UINT32_MAX) return PLAYER_NO_MAP;
    for (int i=0;i<3;++i) if (!isfinite(o->position[i])) return PLAYER_NONFINITE;
    if (!isfinite(o->rotation[1])) return PLAYER_NONFINITE;
    return PLAYER_OK;
}
static PlayerState player_observe(PlayerRead read, void *ctx, uintptr_t image, PlayerObservation *o) {
    PlayerObservation second;
    PlayerState first=player_walk(read,ctx,image,o);
    if (first!=PLAYER_OK) return first;
    PlayerState next=player_walk(read,ctx,image,&second);
    if (next==PLAYER_UNREADABLE || next==PLAYER_NONFINITE) return next;
    if (next!=PLAYER_OK || memcmp(o,&second,sizeof(second))) return PLAYER_CHANGED;
    return PLAYER_OK;
}
static bool player_mach_read(uintptr_t address, void *out, size_t n, void *ctx) {
    (void)ctx;
    mach_vm_size_t copied=0;
    return mach_vm_read_overwrite(mach_task_self(),address,n,(mach_vm_address_t)out,&copied)==KERN_SUCCESS && copied==n;
}
static bool player_code_guard(uintptr_t image, size_t size, PlayerRead read, void *ctx) {
    static const struct { size_t offset,size; uint64_t hash; } guards[]={
        {0x18db3da,0xd,UINT64_C(0x50790872efdc2e75)},
        {0x18db4dc,0x4,UINT64_C(0xd3c975b8cd0084f6)},
        {0x18be0d6,0x25,UINT64_C(0xad831e30e714a8da)},
        {0x18be135,0x8,UINT64_C(0xa79b2f0b44332f16)},
        {0x18f17a9,0xb,UINT64_C(0xf4e62d9a328842db)},
        {0x18f83e0,0xc,UINT64_C(0x2d2f7788f79bbb7b)},
    };
    if (size<0x553e880 || image>UINTPTR_MAX-size) return false;
    for (size_t i=0;i<sizeof(guards)/sizeof(guards[0]);++i) {
        unsigned char bytes[64];
        if (guards[i].offset>size || guards[i].size>size-guards[i].offset ||
            !player_field(read,ctx,image,guards[i].offset,bytes,guards[i].size)) return false;
        uint64_t hash=UINT64_C(14695981039346656037);
        for (size_t j=0;j<guards[i].size;++j) hash=(hash^bytes[j])*UINT64_C(1099511628211);
        if (hash!=guards[i].hash) return false;
    }
    return true;
}
static pthread_mutex_t player_lock=PTHREAD_MUTEX_INITIALIZER;
static _Atomic bool player_enabled;
static _Atomic uint64_t player_pad_reads;
static char player_path[PATH_MAX];
static uintptr_t player_image;
static uint64_t player_sequence,player_started,player_last;
static uint64_t player_token;
static int player_last_state=-1;
static _Noreturn void player_stop(const char *reason) {
    fprintf(stderr,"PLAYER-POS STOP: %s\n",reason);
    fflush(stderr);
    _Exit(23);
}
static uint64_t player_now(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC,&ts)) player_stop("monotonic clock failed");
    return (uint64_t)ts.tv_sec*UINT64_C(1000000000)+(uint64_t)ts.tv_nsec;
}
static void player_publish(PlayerState state,const PlayerObservation *o,uint64_t sampled) {
    char record[1024],fields[256]="",temporary[PATH_MAX];
    uint64_t published=player_now();
    if (state==PLAYER_OK) {
        int n=snprintf(fields,sizeof(fields),",\"player\":\"0x%" PRIxPTR "\",\"xyz\":[%.9g,%.9g,%.9g],"
            "\"player_yaw_rad\":%.9g,\"map_raw\":%" PRIu32,o->player,
            (double)o->position[0],(double)o->position[1],(double)o->position[2],(double)o->rotation[1],o->map);
        if (n<0 || (size_t)n>=sizeof(fields)) player_stop("field formatting failed");
    }
    int n=snprintf(record,sizeof(record),"{\"version\":1,\"pid\":%ld,\"run_id\":\"%016" PRIx64 "\","
        "\"started_monotonic_ns\":%" PRIu64 ",\"sequence\":%" PRIu64 ",\"sample_monotonic_ns\":%" PRIu64 ","
        "\"published_monotonic_ns\":%" PRIu64 ",\"sample_age_ns\":%" PRIu64 ",\"pad_reads\":%" PRIu64 ","
        "\"valid\":%s,\"reason\":\"%s\",\"source\":\"pad-read-observation\"%s}\n",
        (long)getpid(),player_token,player_started,++player_sequence,sampled,published,published-sampled,
        atomic_load_explicit(&player_pad_reads,memory_order_relaxed),state==PLAYER_OK ? "true":"false",player_reason(state),fields);
    if (n<0 || (size_t)n>=sizeof(record)) player_stop("record formatting failed");
    int t=snprintf(temporary,sizeof(temporary),"%s.XXXXXX",player_path);
    if (t<0 || (size_t)t>=sizeof(temporary)) player_stop("output path too long");
    int fd=mkstemp(temporary);
    if (fd<0) player_stop("cannot create output temporary file");
    size_t done=0;
    while (done<(size_t)n) {
        ssize_t wrote=write(fd,record+done,(size_t)n-done);
        if (wrote<0 && errno==EINTR) continue;
        if (wrote<=0) { close(fd); unlink(temporary); player_stop("output write failed"); }
        done+=(size_t)wrote;
    }
    if (close(fd)) { unlink(temporary); player_stop("output close failed"); }
    if (rename(temporary,player_path)) { unlink(temporary); player_stop("output rename failed"); }
    if ((int)state!=player_last_state) {
        fprintf(stderr,"PLAYER-POS: %s (sequence %" PRIu64 "); observation only, not a controllability flag\n",player_reason(state),player_sequence);
        player_last_state=state;
    }
}
void runtime_player_position_init(uintptr_t image,size_t size) {
    const char *path=getenv("BB_PLAYER_POS_FILE");
    if (!path || !*path) return;
    if (path[0]!='/' || strlen(path)+8>=sizeof(player_path)) player_stop("BB_PLAYER_POS_FILE requires an absolute bounded path");
    strcpy(player_path,path);
    player_image=image;
    player_started=player_now();
    arc4random_buf(&player_token,sizeof(player_token));
    PlayerObservation empty={0};
    player_publish(PLAYER_STARTING,&empty,player_started);
    if (!player_code_guard(image,size,player_mach_read,NULL)) player_stop("unsupported image: player position code guard failed");
    atomic_store_explicit(&player_enabled,true,memory_order_release);
}
static void player_position_sample(void) {
    if (!atomic_load_explicit(&player_enabled,memory_order_acquire)) return;
    int saved_errno=errno;
    int32_t *guest_errno=runtime_errno();
    int32_t saved_guest_errno=*guest_errno;
    atomic_fetch_add_explicit(&player_pad_reads,1,memory_order_relaxed);
    int cancel_state;
    if (pthread_setcancelstate(PTHREAD_CANCEL_DISABLE,&cancel_state)) player_stop("cannot disable cancellation");
    if (!pthread_mutex_trylock(&player_lock)) {
        uint64_t now=player_now();
        if (!player_last || now-player_last>=UINT64_C(100000000)) {
            PlayerObservation observation;
            PlayerState state=player_observe(player_mach_read,NULL,player_image,&observation);
            player_last=now;
            player_publish(state,&observation,now);
            if (state==PLAYER_UNREADABLE || state==PLAYER_NONFINITE) player_stop(player_reason(state));
        }
        pthread_mutex_unlock(&player_lock);
    }
    *guest_errno=saved_guest_errno;
    errno=saved_errno;
    if (pthread_setcancelstate(cancel_state,NULL)) player_stop("cannot restore cancellation");
}
#endif
