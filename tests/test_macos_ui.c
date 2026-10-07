#ifdef __APPLE__
#include "macos_ui.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>

static atomic_int polls;
static void increment(void *argument) {
    assert(pthread_main_np());
    ++*(int *)argument;
}
static void nested(void *argument) {
    assert(bb_macos_ui_call(increment, argument) == 0);
}
static void poll(void *unused) {
    (void)unused;
    assert(pthread_main_np());
    atomic_fetch_add(&polls, 1);
}
static void install_poll(void *unused) {
    (void)unused;
    assert(bb_macos_ui_set_poll(poll, NULL) == 0);
}
static void clear_poll(void *unused) {
    (void)unused;
    assert(bb_macos_ui_set_poll(NULL, NULL) == 0);
}
static void *caller(void *argument) {
    for (int i = 0; i < 100; ++i) assert(bb_macos_ui_call(increment, argument) == 0);
    return NULL;
}
static int guest(void *argument) {
    assert(!pthread_main_np());
    assert(bb_macos_ui_set_poll(poll, NULL) == EPERM);
    assert(bb_macos_ui_call(NULL, NULL) == EINVAL);
    assert(bb_macos_ui_call(nested, argument) == 0);
    pthread_t callers[3];
    for (int i = 0; i < 3; ++i) assert(pthread_create(&callers[i], NULL, caller, argument) == 0);
    for (int i = 0; i < 3; ++i) assert(pthread_join(callers[i], NULL) == 0);
    assert(bb_macos_ui_call(install_poll, NULL) == 0);
    while (atomic_load(&polls) < 3) {
        struct timespec delay = {0, 1000000};
        nanosleep(&delay, NULL);
    }
    assert(bb_macos_ui_call(clear_poll, NULL) == 0);
    return 23;
}
static int no_ui(void *unused) { (void)unused; return 7; }
static int failed(void *unused) { (void)unused; bb_macos_ui_exit(19); }
static void *unavailable(void *unused) {
    (void)unused;
    int value = 0;
    assert(bb_macos_ui_call(increment, &value) == ENXIO);
    return NULL;
}
int main(void) {
    int value = 0;
    assert(bb_macos_ui_run(guest, &value) == 23);
    assert(value == 301 && atomic_load(&polls) >= 3);
    assert(bb_macos_ui_run(no_ui, NULL) == 7);
    assert(bb_macos_ui_run(failed, NULL) == 19);
    pthread_t thread;
    assert(pthread_create(&thread, NULL, unavailable, NULL) == 0);
    assert(pthread_join(thread, NULL) == 0);
    puts("Darwin main-thread dispatch tests passed");
    return 0;
}
#else
int main(void) { return 0; }
#endif
