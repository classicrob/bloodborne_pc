#ifdef __APPLE__
#include "compat_mac.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_rwlock_t rwlock = PTHREAD_RWLOCK_INITIALIZER;
static struct timespec deadline(clockid_t clock, long ns) {
    struct timespec time;
    assert(clock_gettime(clock, &time) == 0);
    time.tv_nsec += ns;
    time.tv_sec += time.tv_nsec / 1000000000;
    time.tv_nsec %= 1000000000;
    return time;
}
static void *contended(void *unused) {
    (void)unused;
    struct timespec end = deadline(CLOCK_REALTIME, 10000000);
    assert(pthread_mutex_timedlock(&mutex, &end) == ETIMEDOUT);
    end = deadline(CLOCK_REALTIME, 10000000);
    assert(pthread_rwlock_timedrdlock(&rwlock, &end) == ETIMEDOUT);
    end = deadline(CLOCK_REALTIME, 10000000);
    assert(pthread_rwlock_timedwrlock(&rwlock, &end) == ETIMEDOUT);
    assert(compat_thread_id() != 0);
    return NULL;
}
static atomic_int cpu_ready, cpu_stop;
static void *cpu_worker(void *unused) {
    (void)unused;
    atomic_store(&cpu_ready, 1);
    while (!atomic_load(&cpu_stop)) { }
    return NULL;
}
static void test_thread_cpu_clock(void) {
    pthread_t thread;
    assert(pthread_create(&thread, NULL, cpu_worker, NULL) == 0);
    while (!atomic_load(&cpu_ready)) { }
    int64_t user_before, system_before, user_after, system_after;
    uint32_t port = pthread_mach_thread_np(thread);
    assert(compat_thread_cpu_times(port, &user_before, &system_before) == 0);
    struct timespec delay = {0, 20000000};
    assert(nanosleep(&delay, NULL) == 0);
    assert(compat_thread_cpu_times(port, &user_after, &system_after) == 0);
    assert(user_after + system_after > user_before + system_before);
    atomic_store(&cpu_stop, 1);
    assert(pthread_join(thread, NULL) == 0);
}
int main(void) {
    pthread_t thread;
    assert(pthread_mutex_lock(&mutex) == 0);
    assert(pthread_rwlock_wrlock(&rwlock) == 0);
    assert(pthread_create(&thread, NULL, contended, NULL) == 0);
    assert(pthread_join(thread, NULL) == 0);
    assert(pthread_mutex_unlock(&mutex) == 0);
    assert(pthread_rwlock_unlock(&rwlock) == 0);
    struct timespec past = {0, 0};
    assert(pthread_mutex_timedlock(&mutex, &past) == 0);
    pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
    struct timespec end = deadline(CLOCK_MONOTONIC, 10000000);
    assert(compat_cond_timedwait_monotonic(&cond, &mutex, &end) == ETIMEDOUT);
    assert(pthread_mutex_trylock(&mutex) == EBUSY);
    assert(pthread_mutex_unlock(&mutex) == 0);
    assert(pthread_cond_destroy(&cond) == 0);
    assert(pthread_rwlock_timedrdlock(&rwlock, &past) == 0);
    assert(pthread_rwlock_unlock(&rwlock) == 0);
    assert(pthread_rwlock_timedwrlock(&rwlock, &past) == 0);
    assert(pthread_rwlock_unlock(&rwlock) == 0);
    struct timespec invalid = {0, 1000000000};
    assert(pthread_mutex_timedlock(&mutex, &invalid) == EINVAL);
    assert(clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &past, NULL) == 0);
    end = deadline(CLOCK_MONOTONIC, 10000000);
    assert(clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &end, NULL) == 0);
    struct timespec now;
    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    assert(now.tv_sec > end.tv_sec || (now.tv_sec == end.tv_sec && now.tv_nsec >= end.tv_nsec));
    assert(clock_nanosleep(CLOCK_MONOTONIC, 2, &past, NULL) == EINVAL);
    unsigned char random[64] = {0}, zero[64] = {0};
    assert(compat_random(random, sizeof(random)) == 0);
    assert(memcmp(random, zero, sizeof(random)) != 0);
    assert(compat_random(NULL, 1) == -1 && errno == EFAULT);
    int64_t user, kernel;
    assert(compat_cpu_times(0, &user, &kernel) == 0 && user >= 0 && kernel >= 0);
    assert(compat_cpu_times(1, &user, &kernel) == 0 && user >= 0 && kernel >= 0);
    test_thread_cpu_clock();
    puts("Darwin compatibility tests passed");
    return 0;
}
#else
int main(void) { return 0; }
#endif
