// bbport (macOS): the scheduling of the hot GPU-side threads (stage A, stage B draw recording,
// the Vulkan recording threads), read once from the environment:
// - BB_HOT_THREAD_QOS=user-interactive|user-initiated|default: the thread's QoS class, set at its
//   start;
// - BB_TASK_ROLE=foreground|default-app: the process's task role (TASK_CATEGORY_POLICY), set
//   once, before the hot threads are created (Liverpool's constructor).
// Unset or empty leaves today's behaviour: the inherited class, and the role we are launched
// with. Diagnostic A/B arms (docs/macos/hot-thread-scheduling.md): the verifier saw these threads
// move from P-cores to E-cores after ~1-1.5 s of sustained load as an accessory app (role 6), not
// as a regular app (role 2). Measured: the role, not the requested class, sets the Mach base
// priority (31 in role 0 for every class, 46 as default-app, 47 as foreground).
// Any other value, a refused set, or a role applied off the main thread stops: an A/B arm must
// not silently run with the wrong setting. The thread's startup line (bbport_thread_report.h)
// gives the class requested, the Mach priorities and the task role the thread actually has, then
// both settings.
#pragma once
#ifdef __APPLE__
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mach/mach.h>
#include <mutex>
#include <optional>
#include <pthread.h>
#include <pthread/qos.h>
#include "bbport_thread_report.h"

namespace BbHotThreadQos {

/// The class a BB_HOT_THREAD_QOS value names; nullopt for an unknown value.
inline std::optional<qos_class_t> Parse(const char* value) {
    if (!std::strcmp(value, "user-interactive")) {
        return QOS_CLASS_USER_INTERACTIVE;
    }
    if (!std::strcmp(value, "user-initiated")) {
        return QOS_CLASS_USER_INITIATED;
    }
    if (!std::strcmp(value, "default")) {
        return QOS_CLASS_DEFAULT;
    }
    return std::nullopt;
}

/// A BB_HOT_THREAD_QOS value: nullopt when unset or empty (left as inherited); stops on any
/// other value than the three classes.
inline std::optional<qos_class_t> Read(const char* env) {
    if (!env || !*env) {
        return std::nullopt;
    }
    const std::optional<qos_class_t> qos = Parse(env);
    if (!qos) {
        std::fprintf(stderr,
                     "STOP: BB_HOT_THREAD_QOS=%s is not user-interactive, user-initiated or "
                     "default\n",
                     env);
        std::abort();
    }
    return qos;
}

/// BB_HOT_THREAD_QOS, read once (at the first hot thread's start).
inline std::optional<qos_class_t> Configured() {
    static const std::optional<qos_class_t> configured = Read(std::getenv("BB_HOT_THREAD_QOS"));
    return configured;
}

/// The role a BB_TASK_ROLE value names: nullopt when unset or empty (left as launched); stops
/// on any other value.
inline std::optional<task_role_t> ReadRole(const char* env) {
    if (!env || !*env) {
        return std::nullopt;
    }
    if (!std::strcmp(env, "foreground")) {
        return TASK_FOREGROUND_APPLICATION;
    }
    if (!std::strcmp(env, "default-app")) {
        return TASK_DEFAULT_APPLICATION;
    }
    std::fprintf(stderr, "STOP: BB_TASK_ROLE=%s is not foreground or default-app\n", env);
    std::abort();
}

/// BB_TASK_ROLE, read once.
inline std::optional<task_role_t> ConfiguredRole() {
    static const std::optional<task_role_t> configured = ReadRole(std::getenv("BB_TASK_ROLE"));
    return configured;
}

/// Sets this task's role, from the main thread; stops otherwise or if Darwin refuses. Measured
/// (Rosetta and native, one process per case): set from the main thread (directly or through a
/// joined helper), the main thread and threads created afterwards run at the role's base
/// priority. Set by any other thread, that thread and every thread it then creates stay at the
/// old one (31, for 3 s and more). A forked child's threads are never lifted. The role survives
/// exec.
inline void ApplyRole(task_role_t role) {
    if (!pthread_main_np()) {
        std::fprintf(stderr, "STOP: BB_TASK_ROLE must be applied from the main thread (another "
                             "thread, and the threads it creates, keep the old priority)\n");
        std::abort();
    }
    task_category_policy_data_t category{role};
    const kern_return_t kr =
        task_policy_set(mach_task_self(), TASK_CATEGORY_POLICY,
                        reinterpret_cast<task_policy_t>(&category), TASK_CATEGORY_POLICY_COUNT);
    if (kr != KERN_SUCCESS) {
        std::fprintf(stderr, "STOP: Darwin task role %s failed (%d)\n",
                     BbThreadReport::RoleName(role), kr);
        std::abort();
    }
}

/// BB_TASK_ROLE, applied once: from Liverpool's constructor (on the main thread: main,
/// probe_main, bbgpu_init, RegisterLib), before it starts stage A and before the presenter
/// starts stage B and the Vulkan recording threads.
inline void ApplyConfiguredRole() {
    static std::once_flag once;
    std::call_once(once, [] {
        if (const std::optional<task_role_t> role = ConfiguredRole()) {
            ApplyRole(*role);
        }
    });
}

/// Sets the calling thread's class to `qos`; stops if Darwin refuses.
inline void Apply(qos_class_t qos) {
    if (const int error = pthread_set_qos_class_self_np(qos, 0)) {
        std::fprintf(stderr, "STOP: Darwin hot-thread QoS %s failed (%d)\n",
                     BbThreadReport::QosName(qos), error);
        std::abort();
    }
}

/// At a hot thread's start: applies BB_HOT_THREAD_QOS (`qos`), then prints the thread's
/// startup line with both settings.
inline void Start(const char* name, std::optional<qos_class_t> qos = Configured()) {
    if (qos) {
        Apply(*qos);
    }
    const std::optional<task_role_t> role = ConfiguredRole();
    std::printf("%s; BB_HOT_THREAD_QOS %s; BB_TASK_ROLE %s\n",
                BbThreadReport::Describe(name).c_str(),
                qos ? BbThreadReport::QosName(*qos) : "unset (inherited)",
                role ? BbThreadReport::RoleName(*role) : "unset (as launched)");
    std::fflush(stdout);
}

} // namespace BbHotThreadQos
#endif
