#ifndef BB_MACOS_UI_H
#define BB_MACOS_UI_H
#ifdef __APPLE__
#ifdef __cplusplus
extern "C" {
#endif

typedef void (*BBMacUiTask)(void *argument);
/* Runs the guest worker while the process main thread handles UI requests.
 * Returns the worker's exit status. */
int bb_macos_ui_run(int (*worker)(void *argument), void *argument);
/* Synchronous dispatch: 0 on success, otherwise a positive errno value. */
int bb_macos_ui_call(BBMacUiTask task, void *argument);
/* Main thread only. A NULL task clears event polling. */
int bb_macos_ui_set_poll(BBMacUiTask task, void *argument);
/* A loader failure on the guest worker wakes the main loop and sets its status.
 * Calls from other threads terminate the process. */
__attribute__((noreturn)) void bb_macos_ui_exit(int status);

#ifdef __cplusplus
}
#endif
#endif
#endif
