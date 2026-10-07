#ifdef __APPLE__
#include "macos_ui.h"
#include <pthread.h>
#include <errno.h>
#include <stdlib.h>
#include <time.h>

typedef struct UiRequest {
    BBMacUiTask task;
    void *argument;
    int done, error;
    pthread_cond_t completed;
    struct UiRequest *next;
} UiRequest;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wakeup = PTHREAD_COND_INITIALIZER;
static UiRequest *first, **tail = &first;
static int running, worker_done, worker_status;
static BBMacUiTask poll_events;
static void *poll_argument;
static __thread int is_guest_worker;

typedef struct { int (*function)(void *); void *argument; } Worker;
static void complete_worker(int status) {
    pthread_mutex_lock(&lock);
    worker_status = status;
    worker_done = 1;
    pthread_cond_signal(&wakeup);
    pthread_mutex_unlock(&lock);
}
static void *run_worker(void *argument) {
    Worker *worker = argument;
    is_guest_worker = 1;
    int status = worker->function(worker->argument);
    complete_worker(status);
    return NULL;
}
void bb_macos_ui_exit(int status) {
    if (!is_guest_worker) exit(status);
    complete_worker(status);
    pthread_exit(NULL);
}
int bb_macos_ui_call(BBMacUiTask task, void *argument) {
    if (!task) return EINVAL;
    if (pthread_main_np()) { task(argument); return 0; }
    UiRequest request = {.task = task, .argument = argument};
    int error = pthread_cond_init(&request.completed, NULL);
    if (error) return error;
    int old_cancel;
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old_cancel);
    pthread_mutex_lock(&lock);
    if (!running) error = ENXIO;
    else if (worker_done) error = ECANCELED;
    else {
        *tail = &request;
        tail = &request.next;
        pthread_cond_signal(&wakeup);
        while (!request.done) pthread_cond_wait(&request.completed, &lock);
        error = request.error;
    }
    pthread_mutex_unlock(&lock);
    pthread_cond_destroy(&request.completed);
    pthread_setcancelstate(old_cancel, NULL);
    return error;
}
int bb_macos_ui_set_poll(BBMacUiTask task, void *argument) {
    if (!pthread_main_np()) return EPERM;
    poll_events = task;
    poll_argument = argument;
    return 0;
}
int bb_macos_ui_run(int (*worker_function)(void *), void *argument) {
    if (!pthread_main_np() || !worker_function) { errno = EINVAL; return 1; }
    pthread_mutex_lock(&lock);
    if (running) { pthread_mutex_unlock(&lock); errno = EBUSY; return 1; }
    running = 1;
    worker_done = worker_status = 0;
    poll_events = NULL;
    poll_argument = NULL;
    Worker worker = {worker_function, argument};
    pthread_t thread;
    int error = pthread_create(&thread, NULL, run_worker, &worker);
    if (error) {
        running = 0;
        pthread_mutex_unlock(&lock);
        errno = error;
        return 1;
    }
    while (!worker_done) {
        if (first) {
            UiRequest *request = first;
            first = request->next;
            if (!first) tail = &first;
            pthread_mutex_unlock(&lock);
            request->task(request->argument);
            pthread_mutex_lock(&lock);
            request->done = 1;
            pthread_cond_signal(&request->completed);
        } else {
            BBMacUiTask poll = poll_events;
            void *poll_arg = poll_argument;
            pthread_mutex_unlock(&lock);
            if (poll) poll(poll_arg);
            pthread_mutex_lock(&lock);
            if (!first && !worker_done) {
                if (poll_events) {
                    const struct timespec delay = {0, 2000000};
                    pthread_cond_timedwait_relative_np(&wakeup, &lock, &delay);
                } else pthread_cond_wait(&wakeup, &lock);
            }
        }
    }
    running = 0;
    while (first) {
        UiRequest *request = first;
        first = request->next;
        request->error = ECANCELED;
        request->done = 1;
        pthread_cond_signal(&request->completed);
    }
    tail = &first;
    poll_events = NULL;
    poll_argument = NULL;
    int status = worker_status;
    pthread_mutex_unlock(&lock);
    pthread_join(thread, NULL);
    return status;
}
#endif
