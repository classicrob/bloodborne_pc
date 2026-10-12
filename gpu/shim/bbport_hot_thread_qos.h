// bbport (macOS): the scheduling of the hot GPU-side threads (stage A, stage B draw recording,
// the Vulkan recording threads), read once from the environment:
// - BB_HOT_THREAD_QOS=user-interactive|user-initiated|default: the thread's QoS class, set at its
//   start;
// - BB_TASK_ROLE=foreground|default-app: the process's task role (TASK_CATEGORY_POLICY), set
//   once by the main thread at process start (bbgpu_apply_task_role, from bb-probe's main);
//   Liverpool's constructor checks it is in effect before the hot threads start.
// Unset or empty leaves today's behaviour: the inherited class, and the role we are launched
// with. Diagnostic A/B arms (docs/macos/hot-thread-scheduling.md). Measured through bb-probe's
// startup path (label-check): as launched (role 0) every class runs at legacy priority (base 31);
// with the foreground role the class sets the tier (user-interactive 47, user-initiated 37,
// default 31). So only both together move a hot thread.
// Any other value, a refused set, a role applied off the main thread, or a role not in effect
// when the hot threads start stops: an A/B arm must not silently run with the wrong setting.
// The thread's startup line (bbport_thread_report.h) gives the class requested, the Mach
// priorities and the task role the thread actually has, then both settings.
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
/// (probes, native and Rosetta): set by a thread other than the main thread, that thread and the
/// threads it then creates kept the old priority (3 s and more); a forked child's threads were
/// never lifted.
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

/// BB_TASK_ROLE, applied once: from the process's main thread before it starts any other thread
/// (bb-probe's main, through bbgpu_apply_task_role, before bb_macos_ui_run starts the worker that
/// runs the game and creates the hot threads).
inline void ApplyConfiguredRole() {
    static std::once_flag once;
    std::call_once(once, [] {
        if (const std::optional<task_role_t> role = ConfiguredRole()) {
            ApplyRole(*role);
        }
    });
}

/// Before the hot threads start (Liverpool's constructor): with BB_TASK_ROLE set, the task must
/// already have that role; stops otherwise (the entry point did not apply it at start).
inline void VerifyConfiguredRole() {
    const std::optional<task_role_t> role = ConfiguredRole();
    if (!role) {
        return;
    }
    task_category_policy_data_t category{};
    mach_msg_type_number_t count = TASK_CATEGORY_POLICY_COUNT;
    boolean_t get_default = false;
    const kern_return_t kr =
        task_policy_get(mach_task_self(), TASK_CATEGORY_POLICY,
                        reinterpret_cast<task_policy_t>(&category), &count, &get_default);
    if (kr != KERN_SUCCESS || category.role != *role) {
        std::fprintf(stderr,
                     "STOP: BB_TASK_ROLE asks for %s but the task role is %s (%d, query %d): "
                     "this entry point did not apply it from its main thread at start "
                     "(bbgpu_apply_task_role)\n",
                     BbThreadReport::RoleName(*role), BbThreadReport::RoleName(category.role),
                     category.role, kr);
        std::abort();
    }
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
