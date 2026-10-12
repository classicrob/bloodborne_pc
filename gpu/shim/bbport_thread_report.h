// bbport (macOS): one startup line per GPU-side thread with its scheduling class, so perf runs
// show what the scheduler was asked for and what the thread got (Astra-2's measurement gap):
// - requested QoS class and relative priority (pthread_get_qos_class_np);
// - effective Mach priority: current, base, maximum (thread_info THREAD_EXTENDED_INFO);
// - the scheduling policy (timeshare or not).
// macOS has no API for the core cluster a thread runs on or may run on (P or E); the line says
// so rather than guess. Printed once per thread, at its start.
#pragma once
#ifdef __APPLE__
#include <cstdio>
#include <mach/mach.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <string>

namespace BbThreadReport {

inline const char* QosName(qos_class_t qos) {
    switch (qos) {
    case QOS_CLASS_USER_INTERACTIVE:
        return "user-interactive";
    case QOS_CLASS_USER_INITIATED:
        return "user-initiated";
    case QOS_CLASS_DEFAULT:
        return "default";
    case QOS_CLASS_UTILITY:
        return "utility";
    case QOS_CLASS_BACKGROUND:
        return "background";
    case QOS_CLASS_UNSPECIFIED:
        return "unspecified";
    }
    return "unknown";
}

/// "Thread <name>: …" for the calling thread.
inline std::string Describe(const char* name) {
    char text[320];
    qos_class_t qos = QOS_CLASS_UNSPECIFIED;
    int relative = 0;
    const int qos_error = pthread_get_qos_class_np(pthread_self(), &qos, &relative);
    thread_extended_info_data_t info{};
    mach_msg_type_number_t count = THREAD_EXTENDED_INFO_COUNT;
    const mach_port_t self = mach_thread_self();
    const kern_return_t kr =
        thread_info(self, THREAD_EXTENDED_INFO, reinterpret_cast<thread_info_t>(&info), &count);
    mach_port_deallocate(mach_task_self(), self);
    if (qos_error || kr != KERN_SUCCESS) {
        std::snprintf(text, sizeof(text), "Thread %s: scheduling query failed (qos %d, "
                      "thread_info %d)", name, qos_error, kr);
    } else {
        std::snprintf(text, sizeof(text),
                      "Thread %s: requested QoS %s (relative %d); Mach priority current %d, "
                      "base %d, max %d; policy %s; core cluster not queryable on macOS",
                      name, QosName(qos), relative, info.pth_curpri, info.pth_priority,
                      info.pth_maxpriority,
                      info.pth_policy == POLICY_TIMESHARE ? "timeshare" : "other");
    }
    return text;
}

/// Prints Describe(name) once, at the calling thread's start.
inline void Report(const char* name) {
    std::printf("%s\n", Describe(name).c_str());
    std::fflush(stdout);
}

} // namespace BbThreadReport
#endif
