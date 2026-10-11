/* Linux pthread-backed opaque PS4 mutex handles. No guest threads yet. */
#define _GNU_SOURCE
#include "runtime.h"
#include "runtime_hle_trace.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
#include <stdatomic.h>
typedef struct { int type; } GuestAttr;
typedef struct { pthread_mutex_t native; } GuestMutex;
#include "runtime_sync_diag.h"
static _Atomic size_t created, locks, unlocks;
static int32_t orbis_error(int e) {
    if (!e) return 0;
    unsigned code;
    switch (e) {
    case EPERM: code=1; break;
    case ENOMEM: code=12; break;
    case EBUSY: code=16; break;
    case EINVAL: code=22; break;
    case EAGAIN: code=35; break;
    case EDEADLK: code=11; break;
    default: fprintf(stderr, "STOP: unmapped host pthread error %d\n", e); exit(21);
    }
    return (int32_t)(UINT32_C(0x80020000) | code);
}
static ABI int32_t attr_init(GuestAttr **out) {
    if (!out) return orbis_error(EINVAL);
    GuestAttr *attr = runtime_guest_malloc(sizeof(*attr));
    if (!attr) return orbis_error(ENOMEM);
    attr->type = 1; *out = attr; return 0;
}
static ABI int32_t attr_type(GuestAttr **attr, int type) {
    if (!attr || !*attr || type < 1 || type > 4) return orbis_error(EINVAL);
    (*attr)->type = type; return 0;
}
/* Priority protocols need a PS4 scheduler; the value is validated and recorded only. */
static ABI int32_t attr_protocol(GuestAttr **attr, int protocol) {
    if (!attr || !*attr || protocol < 0 || protocol > 2) return orbis_error(EINVAL);
    return 0;
}
static ABI int32_t attr_destroy(GuestAttr **attr) {
    if (!attr || !*attr) return orbis_error(EINVAL);
    runtime_guest_free(*attr); *attr = NULL; return 0;
}
static ABI int32_t mutex_init(GuestMutex **out, GuestAttr **attr, const char *name) {
    (void)name;
    if (!out || (attr && !*attr)) return orbis_error(EINVAL);
    int type = attr ? (*attr)->type : 1;
    if (type < 1 || type > 4) return orbis_error(EINVAL);
    GuestMutex *mutex = runtime_guest_malloc(sizeof(*mutex));
    if (!mutex) return orbis_error(ENOMEM);
    pthread_mutexattr_t native_attr;
    int e = SYNC_NATIVE(pthread_mutexattr_init(&native_attr));
    if (e) { runtime_guest_free(mutex); return orbis_error(e); }
    int native_type = type == 2 ? PTHREAD_MUTEX_RECURSIVE : type == 3 ? PTHREAD_MUTEX_NORMAL : PTHREAD_MUTEX_ERRORCHECK;
    e = SYNC_NATIVE(pthread_mutexattr_settype(&native_attr, native_type));
    if (!e) e = SYNC_NATIVE(pthread_mutex_init(&mutex->native, &native_attr));
    pthread_mutexattr_destroy(&native_attr);
    if (e) { runtime_guest_free(mutex); return orbis_error(e); }
    *out = mutex; atomic_fetch_add_explicit(&created,1,memory_order_relaxed); return 0;
}
static pthread_mutex_t static_init = PTHREAD_MUTEX_INITIALIZER;
/* Statically initialized handles (0/1) are created once, even under contention. */
static int32_t ensure_mutex(GuestMutex **mutex) {
    if (!mutex) return orbis_error(EINVAL);
    uintptr_t value = __atomic_load_n((uintptr_t *)mutex, __ATOMIC_ACQUIRE);
    if (value == 2) return orbis_error(EINVAL);
    if (value >= 2) return 0;
    pthread_mutex_lock(&static_init);
    int32_t e = 0;
    if ((uintptr_t)*mutex < 2) {
        GuestMutex *created_mutex = NULL;
        e = mutex_init(&created_mutex, NULL, NULL);
        if (!e) __atomic_store_n(mutex, created_mutex, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&static_init);
    return e;
}
static ABI int32_t mutex_lock(GuestMutex **mutex) {
    int32_t e = ensure_mutex(mutex);
    if (e) return e;
    e = orbis_error(SYNC_NATIVE(pthread_mutex_lock(&(*mutex)->native)));
    if (!e) atomic_fetch_add_explicit(&locks,1,memory_order_relaxed);
    return e;
}
static ABI int32_t mutex_trylock(GuestMutex **mutex) {
    int32_t e = ensure_mutex(mutex);
    if (e) return e;
    e = orbis_error(SYNC_NATIVE(pthread_mutex_trylock(&(*mutex)->native)));
    if (!e) atomic_fetch_add_explicit(&locks,1,memory_order_relaxed);
    return e;
}
static ABI int32_t mutex_unlock(GuestMutex **mutex) {
    if (!mutex || (uintptr_t)*mutex == 2) return orbis_error(EINVAL);
    if ((uintptr_t)*mutex < 2) return orbis_error(EPERM);
    int32_t e = orbis_error(SYNC_NATIVE(pthread_mutex_unlock(&(*mutex)->native)));
    if (!e) atomic_fetch_add_explicit(&unlocks,1,memory_order_relaxed);
    return e;
}
static ABI int32_t mutex_destroy(GuestMutex **mutex) {
    if (!mutex || (uintptr_t)*mutex == 2) return orbis_error(EINVAL);
    if ((uintptr_t)*mutex < 2) return 0;
    int e = SYNC_NATIVE(pthread_mutex_destroy(&(*mutex)->native));
    if (!e) { runtime_guest_free(*mutex); *mutex = (GuestMutex *)(uintptr_t)2; }
    return orbis_error(e);
}
static int deadline_after(struct timespec *end, uint64_t usec) {
    if (clock_gettime(CLOCK_REALTIME, end)) return errno;
    uint64_t ns = (uint64_t)end->tv_nsec + (usec % 1000000) * 1000;
    end->tv_sec += (time_t)(usec / 1000000 + ns / 1000000000);
    end->tv_nsec = (long)(ns % 1000000000);
    return 0;
}
static int32_t timed_error(int e) { return e == ETIMEDOUT ? (int32_t)UINT32_C(0x8002003c) : orbis_error(e); }
static ABI int32_t mutex_timedlock(GuestMutex **mutex, uint32_t usec) {
    int32_t e = ensure_mutex(mutex);
    if (e) return e;
    struct timespec end;
    if (deadline_after(&end, usec)) return orbis_error(EINVAL);
    e = timed_error(SYNC_NATIVE(pthread_mutex_timedlock(&(*mutex)->native, &end)));
    if (!e) atomic_fetch_add_explicit(&locks,1,memory_order_relaxed);
    return e;
}
/* Condition variables: opaque handles over host pthread_cond (CLOCK_REALTIME,
 * like FreeBSD's default), sharing the host mutex behind GuestMutex.
 * `notified` counts signal/broadcast attempts on this cond (BB_COND_WAKE_CHECK). */
typedef struct { pthread_cond_t native; uint64_t id; _Atomic uint64_t notified; } GuestCond;
static _Atomic size_t conds, waits, wakeups;

/* BB_COND_WAKE_CHECK=1 (macOS diagnostic; waits behave the same either way): a wait that
 * returns success although no signal or broadcast was attempted on its cond since it started
 * is an "unnotified wake candidate" (a host spurious wake, e.g. psynch interrupted by a signal).
 * A changed count does not prove this waiter was the one woken: one signal can overlap several
 * waiters. Nothing is enrolled per waiter (a cancelled wait leaves nothing behind) and no
 * diagnostic lock is held across a wait. */
static int wake_check;
static pthread_once_t wake_check_once=PTHREAD_ONCE_INIT;
static _Atomic uint64_t cond_ids, checked_waits, unnotified, failed_notifications;
static void wake_check_configure(void) {
    const char *setting=getenv("BB_COND_WAKE_CHECK");
    if (!setting || !*setting || !strcmp(setting,"0")) return;
    if (strcmp(setting,"1")) { fprintf(stderr,"STOP: BB_COND_WAKE_CHECK must be 0 or 1\n"); abort(); }
#ifdef __APPLE__
    wake_check=1;
    printf("Runtime: BB_COND_WAKE_CHECK=1: reporting condition waits that return without a "
           "signal or broadcast on their cond\n");
    fflush(stdout);
#else
    fprintf(stderr,"STOP: BB_COND_WAKE_CHECK is implemented on macOS only\n"); abort();
#endif
}
static int wake_checking(void) { pthread_once(&wake_check_once,wake_check_configure); return wake_check; }
#ifdef __APPLE__
typedef struct { char thread[32]; uintptr_t caller; uint64_t cond, count; } UnnotifiedSite;
static UnnotifiedSite unnotified_sites[64];
static pthread_mutex_t unnotified_lock=PTHREAD_MUTEX_INITIALIZER;
static void report_unnotified_sites_locked(void) {
    printf("COND-UNNOTIFIED summary: %llu candidates in %llu checked successful waits; "
           "%llu failed notifications\n",(unsigned long long)atomic_load(&unnotified),
           (unsigned long long)atomic_load(&checked_waits),
           (unsigned long long)atomic_load(&failed_notifications));
    for (size_t i=0;i<64 && unnotified_sites[i].count;++i)
        printf("  thread \"%s\" caller %#llx cond #%llu: %llu\n",unnotified_sites[i].thread,
               (unsigned long long)unnotified_sites[i].caller,(unsigned long long)unnotified_sites[i].cond,
               (unsigned long long)unnotified_sites[i].count);
    fflush(stdout);
}
/* After a successful wait: a candidate if no notification was attempted since `before`. */
static void note_wake(GuestCond *c, uint64_t before) {
    atomic_fetch_add_explicit(&checked_waits,1,memory_order_relaxed);
    const uint64_t after=atomic_load_explicit(&c->notified,memory_order_acquire);
    if (after!=before) return;
    const uint64_t n=atomic_fetch_add(&unnotified,1)+1;
    const uintptr_t caller=sync_return_slot ? runtime_hle_trace_original_return(sync_return_slot) : 0;
    char name[32]="";
    pthread_getname_np(pthread_self(),name,sizeof(name));
    static uint64_t last_summary_ns;
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    const uint64_t now=(uint64_t)t.tv_sec*1000000000+(uint64_t)t.tv_nsec;
    pthread_mutex_lock(&unnotified_lock);
    for (size_t i=0;i<64;++i) {
        UnnotifiedSite *s=&unnotified_sites[i];
        if (!s->count) { snprintf(s->thread,sizeof(s->thread),"%s",name); s->caller=caller; s->cond=c->id; }
        if (s->caller==caller && s->cond==c->id && !strcmp(s->thread,name)) { ++s->count; break; }
    }
    if (n<=32)
        printf("COND-UNNOTIFIED candidate #%llu: thread \"%s\" caller %#llx cond #%llu, wait returned 0, "
               "notifications before %llu after %llu\n",(unsigned long long)n,name,
               (unsigned long long)caller,(unsigned long long)c->id,(unsigned long long)before,
               (unsigned long long)after);
    if (n==1 || now-last_summary_ns>UINT64_C(10000000000)) {
        last_summary_ns=now;
        report_unnotified_sites_locked();
    }
    pthread_mutex_unlock(&unnotified_lock);
}
#else
static void note_wake(GuestCond *c, uint64_t before) { (void)c; (void)before; }
#endif
static ABI int32_t cond_init(GuestCond **out, void **attr, const char *name) {
    (void)attr; (void)name;
    if (!out) return orbis_error(EINVAL);
    GuestCond *c = runtime_guest_malloc(sizeof(*c));
    if (!c) return orbis_error(ENOMEM);
    int e = SYNC_NATIVE(pthread_cond_init(&c->native, NULL));
    if (e) { runtime_guest_free(c); return orbis_error(e); }
    c->id = atomic_fetch_add_explicit(&cond_ids,1,memory_order_relaxed)+1;
    atomic_init(&c->notified, 0);
    *out = c; atomic_fetch_add_explicit(&conds,1,memory_order_relaxed); return 0;
}
static int32_t ensure_cond(GuestCond **cond) {
    if (!cond) return orbis_error(EINVAL);
    if (__atomic_load_n((uintptr_t *)cond, __ATOMIC_ACQUIRE) >= 2) return 0;
    pthread_mutex_lock(&static_init);
    int32_t e = 0;
    if ((uintptr_t)*cond < 2) {
        GuestCond *c = NULL;
        e = cond_init(&c, NULL, NULL);
        if (!e) __atomic_store_n(cond, c, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&static_init);
    return e;
}
static ABI int32_t cond_destroy(GuestCond **cond) {
    if (!cond) return orbis_error(EINVAL);
    if ((uintptr_t)*cond < 2) return 0;
    int e = SYNC_NATIVE(pthread_cond_destroy(&(*cond)->native));
    if (!e) { runtime_guest_free(*cond); *cond = NULL; }
    return orbis_error(e);
}
static ABI int32_t cond_wait(GuestCond **cond, GuestMutex **mutex) {
    int32_t e = ensure_cond(cond);
    if (e) return e;
    if (!mutex || (uintptr_t)*mutex < 3) return orbis_error(EINVAL);
    atomic_fetch_add_explicit(&waits,1,memory_order_relaxed);
    GuestCond *c = *cond;
    const int check = wake_checking();
    const uint64_t before = check ? atomic_load_explicit(&c->notified,memory_order_acquire) : 0;
    const int native = SYNC_NATIVE(pthread_cond_wait(&c->native, &(*mutex)->native));
    if (check && !native) note_wake(c, before);
    return orbis_error(native);
}
static int32_t cond_wait_until(GuestCond **cond, GuestMutex **mutex, const struct timespec *end) {
    int32_t e = ensure_cond(cond);
    if (e) return e;
    if (!mutex || (uintptr_t)*mutex < 3 || !end) return orbis_error(EINVAL);
    atomic_fetch_add_explicit(&waits,1,memory_order_relaxed);
    GuestCond *c = *cond;
    const int check = wake_checking();
    const uint64_t before = check ? atomic_load_explicit(&c->notified,memory_order_acquire) : 0;
    const int native = SYNC_NATIVE(pthread_cond_timedwait(&c->native, &(*mutex)->native, end));
    if (check && !native) note_wake(c, before);
    return timed_error(native);
}
static ABI int32_t cond_timedwait(GuestCond **cond, GuestMutex **mutex, uint32_t usec) {
    struct timespec end;
    if (deadline_after(&end, usec)) return orbis_error(EINVAL);
    return cond_wait_until(cond, mutex, &end);
}
static ABI int32_t cond_signal(GuestCond **cond) {
    int32_t e = ensure_cond(cond);
    if (e) return e;
    atomic_fetch_add_explicit(&wakeups,1,memory_order_relaxed);
    /* Counted before the native wake, so a waiter it wakes never finds the count unchanged. */
    if (wake_checking()) atomic_fetch_add_explicit(&(*cond)->notified,1,memory_order_release);
    const int native = SYNC_NATIVE(pthread_cond_signal(&(*cond)->native));
    if (native && wake_checking()) atomic_fetch_add_explicit(&failed_notifications,1,memory_order_relaxed);
    return orbis_error(native);
}
static ABI int32_t cond_broadcast(GuestCond **cond) {
    int32_t e = ensure_cond(cond);
    if (e) return e;
    atomic_fetch_add_explicit(&wakeups,1,memory_order_relaxed);
    if (wake_checking()) atomic_fetch_add_explicit(&(*cond)->notified,1,memory_order_release);
    const int native = SYNC_NATIVE(pthread_cond_broadcast(&(*cond)->native));
    if (native && wake_checking()) atomic_fetch_add_explicit(&failed_notifications,1,memory_order_relaxed);
    return orbis_error(native);
}
/* PS4 struct timespec is {int64 sec, int64 nsec}, identical to Linux x86-64. */
typedef struct { int64_t sec, nsec; } GuestTimespec;
static ABI int32_t cond_abs_timedwait(GuestCond **cond, GuestMutex **mutex, const GuestTimespec *abs) {
    if (!abs || abs->nsec < 0 || abs->nsec >= 1000000000) return orbis_error(EINVAL);
    struct timespec end = {.tv_sec = (time_t)abs->sec, .tv_nsec = (long)abs->nsec};
    return cond_wait_until(cond, mutex, &end);
}
/* POSIX pthread APIs return positive guest errno values, unlike scePthread. */
static int32_t posix_result(int32_t result) { return result ? (int32_t)((uint32_t)result & 0xffff) : 0; }
static ABI int32_t posix_attr_init(GuestAttr **out) { return posix_result(attr_init(out)); }
static ABI int32_t posix_attr_type(GuestAttr **attr,int type) { return posix_result(attr_type(attr,type)); }
static ABI int32_t posix_attr_destroy(GuestAttr **attr) { return posix_result(attr_destroy(attr)); }
static ABI int32_t posix_mutex_init(GuestMutex **out,GuestAttr **attr) { return posix_result(mutex_init(out,attr,NULL)); }
static ABI int32_t posix_mutex_destroy(GuestMutex **m) { return posix_result(mutex_destroy(m)); }
static ABI int32_t posix_mutex_lock(GuestMutex **m) { return posix_result(mutex_lock(m)); }
static ABI int32_t posix_mutex_trylock(GuestMutex **m) { return posix_result(mutex_trylock(m)); }
static ABI int32_t posix_mutex_unlock(GuestMutex **m) { return posix_result(mutex_unlock(m)); }
static ABI int32_t posix_cond_init(GuestCond **c, void **a) { return posix_result(cond_init(c, a, NULL)); }
static ABI int32_t posix_cond_destroy(GuestCond **c) { return posix_result(cond_destroy(c)); }
static ABI int32_t posix_cond_wait(GuestCond **c, GuestMutex **m) { return posix_result(cond_wait(c, m)); }
static ABI int32_t posix_cond_timedwait(GuestCond **c, GuestMutex **m, const GuestTimespec *t) { return posix_result(cond_abs_timedwait(c, m, t)); }
static ABI int32_t posix_cond_signal(GuestCond **c) { return posix_result(cond_signal(c)); }
static ABI int32_t posix_cond_broadcast(GuestCond **c) { return posix_result(cond_broadcast(c)); }

SYNC_WRAP(attr_init, (GuestAttr **out), (out), "attribute", out, 0, UINTPTR_MAX, NULL)
SYNC_WRAP(attr_type, (GuestAttr **attr, int type), (attr, type), "attribute", attr, 1, UINTPTR_MAX, NULL)
SYNC_WRAP(attr_protocol, (GuestAttr **attr, int protocol), (attr, protocol), "attribute", attr, 1, UINTPTR_MAX, NULL)
SYNC_WRAP(attr_destroy, (GuestAttr **attr), (attr), "attribute", attr, 1, UINTPTR_MAX, NULL)
SYNC_WRAP(mutex_init, (GuestMutex **out, GuestAttr **attr, const char *name), (out, attr, name), "mutex", out, 0, 2, NULL)
SYNC_WRAP(mutex_lock, (GuestMutex **mutex), (mutex), "mutex", mutex, 1, 2, NULL)
SYNC_WRAP(mutex_trylock, (GuestMutex **mutex), (mutex), "mutex", mutex, 1, 2, NULL)
SYNC_WRAP(mutex_unlock, (GuestMutex **mutex), (mutex), "mutex", mutex, 1, 2, NULL)
SYNC_WRAP(mutex_destroy, (GuestMutex **mutex), (mutex), "mutex", mutex, 1, 2, NULL)
SYNC_WRAP(mutex_timedlock, (GuestMutex **mutex, uint32_t usec), (mutex, usec), "mutex", mutex, 1, 2, NULL)
SYNC_WRAP(cond_init, (GuestCond **out, void **attr, const char *name), (out, attr, name), "condition", out, 0, UINTPTR_MAX, NULL)
SYNC_WRAP(cond_destroy, (GuestCond **cond), (cond), "condition", cond, 1, UINTPTR_MAX, NULL)
SYNC_WRAP(cond_wait, (GuestCond **cond, GuestMutex **mutex), (cond, mutex), "condition", cond, 1, UINTPTR_MAX, mutex)
SYNC_WRAP(cond_timedwait, (GuestCond **cond, GuestMutex **mutex, uint32_t usec), (cond, mutex, usec), "condition", cond, 1, UINTPTR_MAX, mutex)
SYNC_WRAP(cond_signal, (GuestCond **cond), (cond), "condition", cond, 1, UINTPTR_MAX, NULL)
SYNC_WRAP(cond_broadcast, (GuestCond **cond), (cond), "condition", cond, 1, UINTPTR_MAX, NULL)
SYNC_WRAP(posix_attr_init, (GuestAttr **out), (out), "attribute", out, 0, UINTPTR_MAX, NULL)
SYNC_WRAP(posix_attr_type, (GuestAttr **attr,int type), (attr, type), "attribute", attr, 1, UINTPTR_MAX, NULL)
SYNC_WRAP(posix_attr_destroy, (GuestAttr **attr), (attr), "attribute", attr, 1, UINTPTR_MAX, NULL)
SYNC_WRAP(posix_mutex_init, (GuestMutex **out,GuestAttr **attr), (out, attr), "mutex", out, 0, 2, NULL)
SYNC_WRAP(posix_mutex_destroy, (GuestMutex **m), (m), "mutex", m, 1, 2, NULL)
SYNC_WRAP(posix_mutex_lock, (GuestMutex **m), (m), "mutex", m, 1, 2, NULL)
SYNC_WRAP(posix_mutex_trylock, (GuestMutex **m), (m), "mutex", m, 1, 2, NULL)
SYNC_WRAP(posix_mutex_unlock, (GuestMutex **m), (m), "mutex", m, 1, 2, NULL)
SYNC_WRAP(posix_cond_init, (GuestCond **c, void **a), (c, a), "condition", c, 0, UINTPTR_MAX, NULL)
SYNC_WRAP(posix_cond_destroy, (GuestCond **c), (c), "condition", c, 1, UINTPTR_MAX, NULL)
SYNC_WRAP(posix_cond_wait, (GuestCond **c, GuestMutex **m), (c, m), "condition", c, 1, UINTPTR_MAX, m)
SYNC_WRAP(posix_cond_timedwait, (GuestCond **c, GuestMutex **m, const GuestTimespec *t), (c, m, t), "condition", c, 1, UINTPTR_MAX, m)
SYNC_WRAP(posix_cond_signal, (GuestCond **c), (c), "condition", c, 1, UINTPTR_MAX, NULL)
SYNC_WRAP(posix_cond_broadcast, (GuestCond **c), (c), "condition", c, 1, UINTPTR_MAX, NULL)

uintptr_t runtime_mutex_resolve(const char *name) {
    if (!strcmp(name,"0TyVk4MSLt0#I#J")) return (uintptr_t)SYNC_EXPORT(posix_cond_init);
    if (!strcmp(name,"RXXqi4CtF8w#I#J")) return (uintptr_t)SYNC_EXPORT(posix_cond_destroy);
    if (!strcmp(name,"Op8TBGY5KHg#I#J")) return (uintptr_t)SYNC_EXPORT(posix_cond_wait);
    if (!strcmp(name,"27bAgiJmOh0#I#J")) return (uintptr_t)SYNC_EXPORT(posix_cond_timedwait);
    if (!strcmp(name,"2MOy+rUfuhQ#I#J")) return (uintptr_t)SYNC_EXPORT(posix_cond_signal);
    if (!strcmp(name,"mkx2fVhNMsg#I#J")) return (uintptr_t)SYNC_EXPORT(posix_cond_broadcast);
    if (!strcmp(name,"2Tb92quprl0#p#J")) return (uintptr_t)SYNC_EXPORT(cond_init);
    if (!strcmp(name,"g+PZd2hiacg#p#J")) return (uintptr_t)SYNC_EXPORT(cond_destroy);
    if (!strcmp(name,"WKAXJ4XBPQ4#p#J")) return (uintptr_t)SYNC_EXPORT(cond_wait);
    if (!strcmp(name,"BmMjYxmew1w#p#J")) return (uintptr_t)SYNC_EXPORT(cond_timedwait);
    if (!strcmp(name,"kDh-NfxgMtE#p#J")) return (uintptr_t)SYNC_EXPORT(cond_signal);
    if (!strcmp(name,"JGgj7Uvrl+A#p#J")) return (uintptr_t)SYNC_EXPORT(cond_broadcast);
    if (!strcmp(name,"IafI2PxcPnQ#p#J")) return (uintptr_t)SYNC_EXPORT(mutex_timedlock);
    if (!strcmp(name,"dQHWEsJtoE4#I#J")) return (uintptr_t)SYNC_EXPORT(posix_attr_init);
    if (!strcmp(name,"mDmgMOGVUqg#I#J")) return (uintptr_t)SYNC_EXPORT(posix_attr_type);
    if (!strcmp(name,"HF7lK46xzjY#I#J")) return (uintptr_t)SYNC_EXPORT(posix_attr_destroy);
    if (!strcmp(name,"ttHNfU+qDBU#I#J")) return (uintptr_t)SYNC_EXPORT(posix_mutex_init);
    if (!strcmp(name,"ltCfaGr2JGE#I#J")) return (uintptr_t)SYNC_EXPORT(posix_mutex_destroy);
    if (!strcmp(name,"7H0iTOciTLo#I#J")) return (uintptr_t)SYNC_EXPORT(posix_mutex_lock);
    if (!strcmp(name,"K-jXhbt2gn4#I#J")) return (uintptr_t)SYNC_EXPORT(posix_mutex_trylock);
    if (!strcmp(name,"2Z+PpY6CaJg#I#J")) return (uintptr_t)SYNC_EXPORT(posix_mutex_unlock);
    if (!strcmp(name, "F8bUHwAG284#p#J")) return (uintptr_t)SYNC_EXPORT(attr_init);
    if (!strcmp(name, "iMp8QpE+XO4#p#J")) return (uintptr_t)SYNC_EXPORT(attr_type);
    if (!strcmp(name, "smWEktiyyG0#p#J")) return (uintptr_t)SYNC_EXPORT(attr_destroy);
    if (!strcmp(name, "1FGvU0i9saQ#p#J")) return (uintptr_t)SYNC_EXPORT(attr_protocol);
    if (!strcmp(name, "cmo1RIYva9o#p#J")) return (uintptr_t)SYNC_EXPORT(mutex_init);
    if (!strcmp(name, "9UK1vLZQft4#p#J")) return (uintptr_t)SYNC_EXPORT(mutex_lock);
    if (!strcmp(name, "upoVrzMHFeE#p#J")) return (uintptr_t)SYNC_EXPORT(mutex_trylock);
    if (!strcmp(name, "tn3VlD0hG60#p#J")) return (uintptr_t)SYNC_EXPORT(mutex_unlock);
    if (!strcmp(name, "2Of0f+3mhhE#p#J")) return (uintptr_t)SYNC_EXPORT(mutex_destroy);
    return 0;
}
void runtime_mutex_report(void) {
    printf("Runtime: mutexes created=%zu, locks=%zu, unlocks=%zu\n", atomic_load_explicit(&created,memory_order_relaxed),
           atomic_load_explicit(&locks,memory_order_relaxed), atomic_load_explicit(&unlocks,memory_order_relaxed));
    printf("Runtime: condition variables created=%zu, waits=%zu, wakeups=%zu\n", atomic_load_explicit(&conds,memory_order_relaxed),
           atomic_load_explicit(&waits,memory_order_relaxed), atomic_load_explicit(&wakeups,memory_order_relaxed));
#ifdef __APPLE__
    if (wake_check) { pthread_mutex_lock(&unnotified_lock); report_unnotified_sites_locked(); pthread_mutex_unlock(&unnotified_lock); }
#endif
}

