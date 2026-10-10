#ifndef BB_RUNTIME_SYNC_DIAG_H
#define BB_RUNTIME_SYNC_DIAG_H

#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <inttypes.h>

/* Only error paths access this state or inspect handle storage. */
static _Thread_local const char *sync_native_failure;
static inline int sync_native_result(int result, const char *operation) {
    if (__builtin_expect(result == EINVAL, 0)) sync_native_failure = operation;
    return result;
}
#define SYNC_NATIVE(call) sync_native_result((call), #call)

static int sync_read_word(const void *address, uintptr_t *word) {
    mach_vm_size_t copied = 0;
    return address && mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)address,
        sizeof(*word), (mach_vm_address_t)word, &copied) == KERN_SUCCESS && copied == sizeof(*word);
}

static void sync_handle_state(char *out, size_t size, const void *slot, int initialized,
                              uintptr_t destroyed) {
    uintptr_t handle = 0, prefix = 0;
    if (!initialized) {
        snprintf(out, size, "slot=%p state=init-output-unread", slot);
    } else if (!slot || !sync_read_word(slot, &handle)) {
        snprintf(out, size, "slot=%p state=%s", slot, slot ? "unreadable-slot" : "null-slot");
    } else if (handle < 3) {
        snprintf(out, size, "slot=%p handle=%#" PRIxPTR " state=%s", slot, handle,
                 handle == destroyed ? "destroyed" : "static-or-sentinel");
    } else {
        const int readable = sync_read_word((void *)handle, &prefix);
        snprintf(out, size, "slot=%p handle=%#" PRIxPTR " state=%s native_prefix=%#" PRIxPTR,
                 slot, handle, readable ? "object-readable" : "object-unreadable", prefix);
    }
}

static __attribute__((noinline)) void sync_report_invalid(const char *op, const char *kind,
        const void *slot, int initialized, uintptr_t destroyed, const void *mutex_slot,
        int32_t result, void *caller) {
    const int saved_errno = errno;
    char primary[192], secondary[192] = "none", name[64] = "";
    sync_handle_state(primary, sizeof(primary), slot, initialized, destroyed);
    if (mutex_slot) sync_handle_state(secondary, sizeof(secondary), mutex_slot, 1, 2);
    pthread_getname_np(pthread_self(), name, sizeof(name));
    fprintf(stderr, "Runtime sync EINVAL: op=%s kind=%s %s mutex={%s} "
            "caller=%p thread=%p name=%s result=%#x native_errno=%d native_call=%s\n",
            op, kind, primary, secondary, caller, (void *)pthread_self(), name,
            (unsigned)result, sync_native_failure ? EINVAL : 0,
            sync_native_failure ? sync_native_failure : "none(validation)");
    sync_native_failure = NULL;
    errno = saved_errno;
}

/* Resolve the outer ABI wrapper so POSIX forwarding retains the caller address. */
#define SYNC_EXPORT(name) diagnostic_##name
#define SYNC_WRAP(name, params, args, kind, slot, initialized, destroyed, mutex_slot) \
    static ABI __attribute__((noinline)) int32_t diagnostic_##name params { \
        const int32_t result = name args; \
        if (__builtin_expect(((uint32_t)result & 0xffff) == 22, 0)) \
            sync_report_invalid(#name, kind, slot, initialized, destroyed, mutex_slot, result, \
                                __builtin_extract_return_addr(__builtin_return_address(0))); \
        return result; \
    }
#else
#define SYNC_NATIVE(call) (call)
#define SYNC_EXPORT(name) name
#define SYNC_WRAP(name, params, args, kind, slot, initialized, destroyed, mutex_slot)
#endif
#endif
