/* test_alarm(seconds): alarm() for tests whose limit a SIGSTOP of the test does not use up.
 * heavy.sh --cpu-quiet SIGSTOPs build and test jobs for the length of a perf run; alarm() kept
 * counting through the stop and fired as soon as the test resumed (PREVIEW-25a: the fp16 probe's
 * alarm(45) after a 12-minute pause). A forked watchdog (no thread or mapping in the test's own
 * address space) adds up monotonic time with each observed gap clamped to half a second: an
 * approximation of running time, not a measurement of stopped time. A stop of any length, which
 * stops the watchdog too, costs at most 0.5 s; so does any other stall longer than that. At the limit it sends the test
 * SIGALRM, as alarm() would; 0 cancels. Like alarm(), it is not inherited by fork children. */
#pragma once
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static pid_t test_alarm_watchdog, test_alarm_owner;

static inline void test_alarm(unsigned seconds) {
    if (test_alarm_watchdog && test_alarm_owner == getpid()) {
        kill(test_alarm_watchdog, SIGKILL);
        waitpid(test_alarm_watchdog, NULL, 0);
    }
    test_alarm_watchdog = 0;
    if (!seconds) return;
    const pid_t test = getpid();
    const pid_t watchdog = fork();
    if (watchdog < 0) {
        static const char message[] = "test_alarm: cannot fork the watchdog\n";
        if (write(2, message, sizeof message - 1) < 0) {}
        _exit(70);
    }
    if (watchdog) {
        test_alarm_watchdog = watchdog;
        test_alarm_owner = test;
        return;
    }
    /* The watchdog holds no descriptor of the test's (pipes reach EOF when the test closes them). */
    const long descriptors = sysconf(_SC_OPEN_MAX);
    for (long fd = 0; fd < (descriptors > 0 && descriptors < 65536 ? descriptors : 65536); ++fd)
        close((int)fd);
    const long long limit = (long long)seconds * 1000000000ll, step = 100000000ll, most = 500000000ll;
    long long counted = 0;
    struct timespec last, now, pause = {0, step};
    clock_gettime(CLOCK_MONOTONIC, &last);
    while (getppid() == test) {
        nanosleep(&pause, NULL);
        clock_gettime(CLOCK_MONOTONIC, &now);
        const long long delta = (now.tv_sec - last.tv_sec) * 1000000000ll + (now.tv_nsec - last.tv_nsec);
        counted += delta < most ? delta : most;
        last = now;
        if (counted >= limit) {
            kill(test, SIGALRM);
            break;
        }
    }
    _exit(0);
}
