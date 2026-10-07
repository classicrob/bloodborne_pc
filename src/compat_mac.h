#ifndef BB_COMPAT_MAC_H
#define BB_COMPAT_MAC_H
#ifdef __APPLE__
#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
#include <time.h>

#ifndef CLOCK_REALTIME_COARSE
#define CLOCK_REALTIME_COARSE CLOCK_REALTIME
#endif
#ifndef PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP
#define PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP PTHREAD_RECURSIVE_MUTEX_INITIALIZER
#endif
#ifndef TIMER_ABSTIME
#define TIMER_ABSTIME 1
#endif

#ifdef __cplusplus
extern "C" {
#endif

int compat_mutex_timedlock(pthread_mutex_t *mutex, const struct timespec *deadline);
int compat_rwlock_timedrdlock(pthread_rwlock_t *lock, const struct timespec *deadline);
int compat_rwlock_timedwrlock(pthread_rwlock_t *lock, const struct timespec *deadline);
int compat_cond_timedwait_monotonic(pthread_cond_t *cond, pthread_mutex_t *mutex,
                                    const struct timespec *deadline);
int compat_clock_nanosleep(clockid_t clock, int flags, const struct timespec *request,
                          struct timespec *remaining);
int compat_random(void *buffer, size_t size);
int compat_cpu_times(int who, int64_t *user_us, int64_t *kernel_us);
uint64_t compat_thread_id(void);

#ifdef __cplusplus
}
#endif

#define pthread_mutex_timedlock compat_mutex_timedlock
#define pthread_rwlock_timedrdlock compat_rwlock_timedrdlock
#define pthread_rwlock_timedwrlock compat_rwlock_timedwrlock
#define clock_nanosleep compat_clock_nanosleep
#endif
#endif
