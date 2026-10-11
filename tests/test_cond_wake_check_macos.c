/* BB_COND_WAKE_CHECK (src/runtime_mutex.c): guest condition waits that return success without
 * a signal or broadcast on their cond are reported as unnotified wake candidates; waits behave
 * the same with the check on or off. Through the exported scePthread functions (the guest's
 * path), run as: off, on, traced (on, with the HLE trace wrapping scePthreadCondWait).
 * - off: no counting, no report;
 * - on: real signal, broadcast to two waiters, signal to one of two, timed expiry: no candidate;
 *   a native wake that bypasses the guest signal (what a host spurious wake looks like): one
 *   candidate, named with the waiter's guest caller (through the trace's record when traced);
 *   a cancelled wait leaves nothing behind (later waits are counted and reported as before);
 * - a notification counted before the waiter began but woken natively after: ambiguous, not a
 *   candidate (a paused signaler);
 * - broadcast, then destroy and recreate the cond (address reuse) before the woken waiter
 *   reacquires the mutex: the waiter's check uses its own cond's stats, not the new cond's;
 * - errno and the guest errno are kept even if the report clobbers them;
 * - more than 64 sites: the rest are counted as untracked and reported. */
#define _GNU_SOURCE
#define BB_HLE_TRACE_TEST
#include "../src/runtime_hle_trace.c"
#include "../src/runtime_mutex.c"
#include <assert.h>

void hle_trace_publication_hook(void) {}
static _Thread_local int32_t guest_error;
int32_t *runtime_errno(void) { return &guest_error; }
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    for (size_t i=0;i<count;++i) if (!strcmp(table[i].name,name)) return (uintptr_t)table[i].function;
    return 0;
}

typedef int32_t (ABI *Init)(void **, void **, const char *);
typedef int32_t (ABI *Op)(void **);
typedef int32_t (ABI *Wait)(void **, void **);
typedef int32_t (ABI *TimedWait)(void **, void **, uint32_t);
#define TIMEDOUT UINT32_C(0x8002003c)
static Init mutex_create, cond_create;
static Op lock, unlock, signal_one, broadcast_all;
static Wait wait_cond;
static TimedWait timed_wait;
static void *mutex, *cond;
static int mode_on;

static atomic_int waiting, woken;
/* Waits for the cond once; the caller the diagnostic should name is inside this function. */
static __attribute__((noinline)) void *waiter(void *unused) {
    (void)unused;
    assert(!lock(&mutex));
    atomic_fetch_add(&waiting,1);
    const int32_t result=wait_cond(&cond,&mutex);
    assert(!result);
    atomic_fetch_add(&woken,1);
    assert(!unlock(&mutex));
    return NULL;
}
static void until(atomic_int *counter, int value) { while (atomic_load(counter)<value) usleep(1000); }
/* Every waiter is inside the native wait once it has released the guest mutex. */
static void waiters_blocked(int count) {
    until(&waiting,count);
    assert(!lock(&mutex));
    assert(!unlock(&mutex));
    usleep(20000);
}
static void reset(void) { atomic_store(&waiting,0); atomic_store(&woken,0); }

static void cancel_cleanup(void *unused) { (void)unused; assert(!unlock(&mutex)); }

/* The paused signaler: counted started, its native wake held until released. */
static atomic_int pause_reached, pause_release;
static void pause_notification(void) {
    atomic_store(&pause_reached,1);
    while (!atomic_load(&pause_release)) usleep(1000);
}
static void *paused_signaler(void *unused) { (void)unused; assert(!signal_one(&cond)); return NULL; }
static void clobber_errno(void) { errno=99; *runtime_errno()=77; }
/* Cancels the reporting thread while it reports (the cancel is pending until re-enabled). */
static void cancel_reporter(void) { pthread_cancel(pthread_self()); pthread_testcancel(); }
static void *cancelled_reporter(void *unused) {
    (void)unused;
    assert(!lock(&mutex));
    atomic_fetch_add(&waiting,1);
    pthread_cleanup_push(cancel_cleanup,NULL);
    assert(!wait_cond(&cond,&mutex));
    pthread_testcancel(); /* the cancel arranged during the report acts here */
    pthread_cleanup_pop(1);
    abort(); /* not reached */
}
static atomic_int errno_kept;
static void *errno_waiter(void *unused) {
    (void)unused;
    assert(!lock(&mutex));
    atomic_fetch_add(&waiting,1);
    errno=17; *runtime_errno()=23;
    assert(!wait_cond(&cond,&mutex));
    atomic_store(&errno_kept,errno==17 && *runtime_errno()==23);
    assert(!unlock(&mutex));
    return NULL;
}
static void native_wake_of_new_waiter(void *(*body)(void *)) {
    pthread_t t;
    reset();
    assert(!pthread_create(&t,NULL,body,NULL));
    waiters_blocked(1);
    assert(!pthread_cond_signal(&((GuestCond *)cond)->native));
    assert(!pthread_join(t,NULL));
}
static void *cancelled_waiter(void *unused) {
    (void)unused;
    assert(!lock(&mutex));
    atomic_fetch_add(&waiting,1);
    pthread_cleanup_push(cancel_cleanup,NULL);
    wait_cond(&cond,&mutex);
    pthread_cleanup_pop(0);
    abort(); /* not reached: cancelled while waiting */
}

int main(int argc, char **argv) {
    assert(argc==2);
    const char *mode=argv[1];
    mode_on=strcmp(mode,"off")!=0;
    mutex_create=(Init)runtime_mutex_resolve("cmo1RIYva9o#p#J");
    lock=(Op)runtime_mutex_resolve("9UK1vLZQft4#p#J");
    unlock=(Op)runtime_mutex_resolve("tn3VlD0hG60#p#J");
    cond_create=(Init)runtime_mutex_resolve("2Tb92quprl0#p#J");
    signal_one=(Op)runtime_mutex_resolve("kDh-NfxgMtE#p#J");
    broadcast_all=(Op)runtime_mutex_resolve("JGgj7Uvrl+A#p#J");
    wait_cond=(Wait)runtime_mutex_resolve("WKAXJ4XBPQ4#p#J");
    timed_wait=(TimedWait)runtime_mutex_resolve("BmMjYxmew1w#p#J");
    const uintptr_t direct=(uintptr_t)wait_cond;
    wait_cond=(Wait)runtime_hle_trace_wrap("WKAXJ4XBPQ4#p#J","scePthreadCondWait",direct);
    assert(!strcmp(mode,"traced") ? (uintptr_t)wait_cond!=direct : (uintptr_t)wait_cond==direct);
    assert(!mutex_create(&mutex,NULL,"wake-check") && !cond_create(&cond,NULL,"wake-check"));

    /* A real signal. */
    pthread_t a,b;
    reset();
    assert(!pthread_create(&a,NULL,waiter,NULL));
    waiters_blocked(1);
    assert(!signal_one(&cond));
    assert(!pthread_join(a,NULL) && woken==1);

    /* Broadcast to two waiters; then signal one of two (the other keeps waiting). */
    reset();
    assert(!pthread_create(&a,NULL,waiter,NULL) && !pthread_create(&b,NULL,waiter,NULL));
    waiters_blocked(2);
    assert(!broadcast_all(&cond));
    assert(!pthread_join(a,NULL) && !pthread_join(b,NULL) && woken==2);
    reset();
    assert(!pthread_create(&a,NULL,waiter,NULL) && !pthread_create(&b,NULL,waiter,NULL));
    waiters_blocked(2);
    assert(!signal_one(&cond));
    until(&woken,1);
    usleep(50000);
    assert(woken==1);
    assert(!signal_one(&cond));
    assert(!pthread_join(a,NULL) && !pthread_join(b,NULL) && woken==2);

    /* Timed expiry. */
    assert(!lock(&mutex));
    assert((uint32_t)timed_wait(&cond,&mutex,10000)==TIMEDOUT);
    assert(!unlock(&mutex));
    assert(!atomic_load(&unnotified));
    if (mode_on) assert(atomic_load(&checked_waits)==5);
    else assert(!atomic_load(&checked_waits));
    printf("PASS: %s: signal, broadcast, signal one of two, timed expiry: no candidate\n",mode);

    /* A native wake that bypasses the guest signal. */
    reset();
    assert(!pthread_create(&a,NULL,waiter,NULL));
    waiters_blocked(1);
    assert(!pthread_cond_signal(&((GuestCond *)cond)->native));
    assert(!pthread_join(a,NULL) && woken==1);
    if (mode_on) {
        assert(atomic_load(&unnotified)==1);
        const uintptr_t caller=unnotified_sites[0].caller;
        assert(caller>=(uintptr_t)waiter && caller<(uintptr_t)waiter+512);
        assert(unnotified_sites[0].cond==((GuestCond *)cond)->stats->id && unnotified_sites[0].count==1);
        assert(pthread_equal(unnotified_sites[0].thread,a));
        printf("PASS: %s: a wake without a guest signal is one candidate, named with the waiter's "
               "guest caller and cond\n",mode);
    } else {
        assert(!atomic_load(&unnotified));
        printf("PASS: off: the same wake is not counted\n");
    }

    /* A cancelled wait, then a counted wait as before. */
    reset();
    assert(!pthread_create(&a,NULL,cancelled_waiter,NULL));
    waiters_blocked(1);
    assert(!pthread_cancel(a));
    void *result;
    assert(!pthread_join(a,&result) && result==PTHREAD_CANCELED);
    reset();
    assert(!pthread_create(&a,NULL,waiter,NULL));
    waiters_blocked(1);
    assert(!pthread_cond_signal(&((GuestCond *)cond)->native));
    assert(!pthread_join(a,NULL));
    assert(atomic_load(&unnotified)==(mode_on ? 2u : 0u));
    printf("PASS: %s: a cancelled wait leaves nothing behind\n",mode);
    if (!mode_on) return 0;

    /* A notification in progress when the wait began. */
    notify_pause_for_test=pause_notification;
    pthread_t signaler;
    assert(!pthread_create(&signaler,NULL,paused_signaler,NULL));
    while (!atomic_load(&pause_reached)) usleep(1000);
    notify_pause_for_test=NULL;
    reset();
    assert(!pthread_create(&a,NULL,waiter,NULL));
    waiters_blocked(1);
    atomic_store(&pause_release,1);
    assert(!pthread_join(signaler,NULL) && !pthread_join(a,NULL));
    assert(atomic_load(&unnotified)==2 && atomic_load(&ambiguous)==1);
    printf("PASS: %s: a notification counted before the wait began and woken after: ambiguous, "
           "not a candidate\n",mode);

    /* Broadcast, then destroy and recreate the cond before the woken waiter reacquires. */
    Op cond_destroy_op=(Op)runtime_mutex_resolve("g+PZd2hiacg#p#J");
    reset();
    assert(!pthread_create(&a,NULL,waiter,NULL));
    waiters_blocked(1);
    const CondStats *old_stats=((GuestCond *)cond)->stats;
    const uint64_t checked=atomic_load(&checked_waits);
    assert(!lock(&mutex));
    assert(!broadcast_all(&cond));
    usleep(20000); /* the waiter is woken, now waiting for the mutex */
    const int32_t destroyed=cond_destroy_op(&cond);
    if (!destroyed) {
        assert(!cond_create(&cond,NULL,"wake-check-2"));
        assert(((GuestCond *)cond)->stats!=old_stats);
    }
    assert(!unlock(&mutex));
    assert(!pthread_join(a,NULL));
    assert(atomic_load(&checked_waits)==checked+1 && atomic_load(&unnotified)==2);
    if (!destroyed) {
        assert(!atomic_load(&((GuestCond *)cond)->stats->started));
        printf("PASS: %s: cond destroyed and recreated before the woken waiter reacquired: its "
               "check used its own cond's stats\n",mode);
    } else {
        printf("UNEXERCISED: %s: Darwin refused destroying the cond before the woken waiter "
               "reacquired (%#x)\n",mode,(unsigned)destroyed);
        assert(!cond_create(&cond,NULL,"wake-check-2"));
    }

    /* errno and the guest errno kept although the report clobbers them. */
    report_hook_for_test=clobber_errno;
    native_wake_of_new_waiter(errno_waiter);
    report_hook_for_test=NULL;
    assert(atomic_load(&errno_kept) && atomic_load(&unnotified)==3);
    printf("PASS: %s: errno and the guest errno kept around the report\n",mode);

    /* A reporter cancelled while it reports: its cleanup releases the guest mutex, and the
     * diagnostic's lock is free (another candidate is reported, the summary completes). */
    report_hook_for_test=cancel_reporter;
    reset();
    assert(!pthread_create(&a,NULL,cancelled_reporter,NULL));
    waiters_blocked(1);
    assert(!pthread_cond_signal(&((GuestCond *)cond)->native));
    void *cancelled;
    assert(!pthread_join(a,&cancelled) && cancelled==PTHREAD_CANCELED);
    report_hook_for_test=NULL;
    assert(atomic_load(&unnotified)==4);
    native_wake_of_new_waiter(waiter);
    assert(atomic_load(&unnotified)==5);
    runtime_mutex_report();
    printf("PASS: %s: a reporter cancelled during its report leaves the diagnostic and the guest "
           "mutex free\n",mode);

    /* More than 64 sites: a new cond each (a joined thread's pthread_t can be reused). */
    for (int i=0;i<66;++i) {
        assert(!cond_create(&cond,NULL,"wake-check-site"));
        native_wake_of_new_waiter(waiter);
    }
    assert(atomic_load(&unnotified)==71 && atomic_load(&untracked)>=1);
    pthread_mutex_lock(&unnotified_lock);
    report_unnotified_sites_locked();
    pthread_mutex_unlock(&unnotified_lock);
    printf("PASS: %s: candidates at sites beyond the 64 tracked are counted (%llu)\n",mode,
           (unsigned long long)atomic_load(&untracked));
    return 0;
}
