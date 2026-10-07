#ifdef __APPLE__
#include "compat_mac.h"
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/resource.h>
#include <mach/mach.h>

static int valid_time(const struct timespec *time) {
    return time && time->tv_nsec >= 0 && time->tv_nsec < 1000000000;
}
static int until(clockid_t clock, const struct timespec *deadline, struct timespec *remaining) {
    struct timespec now;
    if (clock_gettime(clock, &now)) return errno;
    if (now.tv_sec > deadline->tv_sec ||
        (now.tv_sec == deadline->tv_sec && now.tv_nsec >= deadline->tv_nsec)) return ETIMEDOUT;
    remaining->tv_sec = deadline->tv_sec - now.tv_sec;
    remaining->tv_nsec = deadline->tv_nsec - now.tv_nsec;
    if (remaining->tv_nsec < 0) { --remaining->tv_sec; remaining->tv_nsec += 1000000000; }
    return 0;
}
/* Darwin has no timed mutex/rwlock entry points. Try before checking the deadline,
 * as POSIX permits acquiring an available lock even when its deadline has passed. */
static int poll_lock(void *lock, const struct timespec *deadline, int kind) {
    if (!valid_time(deadline)) return EINVAL;
    for (;;) {
        int error = kind == 0 ? pthread_mutex_trylock(lock) :
                    kind == 1 ? pthread_rwlock_tryrdlock(lock) : pthread_rwlock_trywrlock(lock);
        if (error != EBUSY && error != EAGAIN) return error;
        struct timespec remaining;
        error = until(CLOCK_REALTIME, deadline, &remaining);
        if (error) return error;
        if (remaining.tv_sec || remaining.tv_nsec > 1000000)
            remaining = (struct timespec){0, 1000000};
        nanosleep(&remaining, NULL);
    }
}
int compat_mutex_timedlock(pthread_mutex_t *mutex, const struct timespec *deadline) {
    return poll_lock(mutex, deadline, 0);
}
int compat_rwlock_timedrdlock(pthread_rwlock_t *lock, const struct timespec *deadline) {
    return poll_lock(lock, deadline, 1);
}
int compat_rwlock_timedwrlock(pthread_rwlock_t *lock, const struct timespec *deadline) {
    return poll_lock(lock, deadline, 2);
}
int compat_cond_timedwait_monotonic(pthread_cond_t *cond, pthread_mutex_t *mutex,
                                    const struct timespec *deadline) {
    if (!valid_time(deadline)) return EINVAL;
    struct timespec remaining;
    int error = until(CLOCK_MONOTONIC, deadline, &remaining);
    if (error == ETIMEDOUT) remaining = (struct timespec){0, 0};
    else if (error) return error;
    return pthread_cond_timedwait_relative_np(cond, mutex, &remaining);
}
int compat_clock_nanosleep(clockid_t clock, int flags, const struct timespec *request,
                          struct timespec *remaining) {
    if (!valid_time(request) || request->tv_sec < 0 || (flags != 0 && flags != TIMER_ABSTIME)) return EINVAL;
    if (clock != CLOCK_REALTIME && clock != CLOCK_MONOTONIC) return EINVAL;
    if (!flags) return nanosleep(request, remaining) ? errno : 0;
    struct timespec delay;
    int error = until(clock, request, &delay);
    if (error == ETIMEDOUT) return 0;
    if (error) return error;
    return nanosleep(&delay, NULL) ? errno : 0;
}
int compat_random(void *buffer, size_t size) {
    if (!buffer && size) { errno = EFAULT; return -1; }
    arc4random_buf(buffer, size);
    return 0;
}
uint64_t compat_thread_id(void) {
    uint64_t id = 0;
    int error = pthread_threadid_np(NULL, &id);
    if (error || !id) { fprintf(stderr, "STOP: Darwin thread ID query failed (%d)\n", error); abort(); }
    return id;
}
int compat_cpu_times(int who, int64_t *user_us, int64_t *kernel_us) {
    if (!who) {
        struct rusage usage;
        if (getrusage(RUSAGE_SELF, &usage)) return -1;
        *user_us = (int64_t)usage.ru_utime.tv_sec * 1000000 + usage.ru_utime.tv_usec;
        *kernel_us = (int64_t)usage.ru_stime.tv_sec * 1000000 + usage.ru_stime.tv_usec;
        return 0;
    }
    if (who != 1) { errno = EINVAL; return -1; }
    mach_port_t thread = mach_thread_self();
    int result = compat_thread_cpu_times(thread, user_us, kernel_us);
    mach_port_deallocate(mach_task_self(), thread);
    return result;
}
int compat_thread_cpu_times(uint32_t mach_thread, int64_t *user_us, int64_t *kernel_us) {
    thread_basic_info_data_t info;
    mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
    kern_return_t result = thread_info(mach_thread, THREAD_BASIC_INFO, (thread_info_t)&info, &count);
    if (result != KERN_SUCCESS) { errno = EIO; return -1; }
    *user_us = (int64_t)info.user_time.seconds * 1000000 + info.user_time.microseconds;
    *kernel_us = (int64_t)info.system_time.seconds * 1000000 + info.system_time.microseconds;
    return 0;
}
#endif
