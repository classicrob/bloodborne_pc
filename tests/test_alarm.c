/* test_alarm (tests/test_alarm.h): fires after the running time, not across a SIGSTOP of the
 * test and its watchdog (heavy.sh --cpu-quiet), cancels, and holds none of the test's pipes. */
#define _DEFAULT_SOURCE /* fork, kill, setpgid, clock_gettime with -std=c11 on glibc */
#include "test_alarm.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
static double now(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec/1e9;
}
static void nap(double seconds) {
    struct timespec t={(time_t)seconds,(long)((seconds-(time_t)seconds)*1e9)}; nanosleep(&t,NULL);
}
/* Runs body in a child in its own process group (so the watchdog stops with it); returns the
 * wait status. stop>0: SIGSTOP the group after 0.3 s for stop seconds. */
static int child(void (*body)(void), double stop) {
    const pid_t pid=fork(); assert(pid>=0);
    if (!pid) { setpgid(0,0); body(); _exit(0); }
    setpgid(pid,pid);
    if (stop>0) {
        nap(0.3); assert(!kill(-pid,SIGSTOP));
        nap(stop); assert(!kill(-pid,SIGCONT));
    }
    int status; assert(waitpid(pid,&status,0)==pid);
    return status;
}
static void fires(void) { test_alarm(1); nap(5); _exit(3); }
static void paused(void) { test_alarm(2); nap(1.2); }
static void cancelled(void) { test_alarm(1); test_alarm(0); nap(1.8); }
static void rearmed(void) { test_alarm(30); test_alarm(1); nap(5); _exit(3); }
static void pipes(void) {
    int p[2]; assert(!pipe(p));
    test_alarm(5);
    close(p[1]);
    char c; assert(read(p[0],&c,1)==0); /* EOF: the watchdog holds no write end */
}
int main(void) {
    double start=now();
    int status=child(fires,0);
    assert(WIFSIGNALED(status) && WTERMSIG(status)==SIGALRM);
    assert(now()-start>0.9 && now()-start<3);
    puts("PASS fires after 1 s of running time (SIGALRM)");
    /* Stopped 3 s with a 2 s limit and 1.5 s of its own running: alarm() would have fired. */
    status=child(paused,3);
    if (!WIFEXITED(status) || WEXITSTATUS(status)) {
        fprintf(stderr,"FAIL a 3 s stop used up a 2 s limit: status %#x\n",status); return 1;
    }
    puts("PASS a SIGSTOP of the test and its watchdog does not count");
    status=child(cancelled,0);
    assert(WIFEXITED(status) && !WEXITSTATUS(status));
    puts("PASS test_alarm(0) cancels");
    start=now();
    status=child(rearmed,0);
    assert(WIFSIGNALED(status) && WTERMSIG(status)==SIGALRM && now()-start<3);
    puts("PASS re-arming replaces the limit");
    status=child(pipes,0);
    assert(WIFEXITED(status) && !WEXITSTATUS(status));
    puts("PASS the watchdog holds none of the test's descriptors");
    return 0;
}
