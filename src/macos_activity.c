#ifdef __APPLE__
#include "macos_activity.h"
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* libobjc exports these (what @autoreleasepool compiles to); no public header declares them. */
void *objc_autoreleasePoolPush(void);
void objc_autoreleasePoolPop(void *pool);
/* NSActivityOptions (Foundation/NSProcessInfo.h). */
#define ACTIVITY_IDLE_SYSTEM_SLEEP_DISABLED (1ull << 20)
#define ACTIVITY_USER_INITIATED (0x00FFFFFFull | ACTIVITY_IDLE_SYSTEM_SLEEP_DISABLED)
#define ACTIVITY_LATENCY_CRITICAL 0xFF00000000ull

static __attribute__((noreturn)) void refuse(const char *why) {
    fprintf(stderr, "Runtime: App Nap: %s\n", why);
    exit(1);
}

void bb_macos_begin_game_activity(void) {
    const char *setting = getenv("BB_APP_NAP");
    if (setting && !strcmp(setting, "allow")) {
        puts("Runtime: App Nap allowed (BB_APP_NAP=allow): no activity, the system may throttle a background game");
        return;
    }
    if (setting) refuse("BB_APP_NAP must be unset (no nap) or \"allow\"");
    /* A background-window game (BB_WINDOW_BACKGROUND=1, every scripted run) is otherwise a
     * candidate for App Nap and QoS demotion after a while. The activity lasts until exit. */
    void *pool = objc_autoreleasePoolPush();
    Class process_info = objc_getClass("NSProcessInfo"), string = objc_getClass("NSString");
    if (!process_info || !string) refuse("Foundation is not loaded (NSProcessInfo, NSString)");
    id info = ((id (*)(Class, SEL))objc_msgSend)(process_info, sel_registerName("processInfo"));
    id reason = ((id (*)(Class, SEL, const char *))objc_msgSend)(
        string, sel_registerName("stringWithUTF8String:"), "game running");
    id activity = info && reason
        ? ((id (*)(id, SEL, uint64_t, id))objc_msgSend)(info, sel_registerName("beginActivityWithOptions:reason:"),
              ACTIVITY_USER_INITIATED | ACTIVITY_LATENCY_CRITICAL, reason)
        : NULL;
    if (!activity) refuse("NSProcessInfo refused the activity");
    ((id (*)(id, SEL))objc_msgSend)(activity, sel_registerName("retain")); /* held until exit */
    objc_autoreleasePoolPop(pool);
    puts("Runtime: App Nap off: NSProcessInfo activity (user initiated, latency critical) for the run");
    fflush(stdout);
}
#endif
