/* Guest call chain by frame pointers, for diagnostics (macOS).
 *
 * The eboot keeps frame pointers (push rbp; mov rbp, rsp), so from a frame inside the guest's call
 * stack [frame] is the caller's frame and [frame + 8] the return address into it. The walk only
 * stops at the first frame that is not strictly above the previous one, and reads frames outside
 * the thread's pthread stack through the kernel, so a bad frame ends it instead of faulting.
 * Header-only: the GPU library and the runtime share it without a host import.
 */
#ifndef BB_GUEST_BACKTRACE_H
#define BB_GUEST_BACKTRACE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>

/* Reads a frame record ([frame] = caller's frame, [frame + 8] = return address). Within the
 * calling thread's pthread stack directly; elsewhere (guest code the runtime runs on a low stack:
 * the main thread, HLE callback workers) through the kernel, so an unmapped frame fails instead
 * of faulting. */
static inline int guest_backtrace_frame(uintptr_t frame, uintptr_t bottom, uintptr_t top,
                                        uintptr_t record[2]) {
    if (frame >= bottom && frame <= top - 16) {
        record[0] = ((const uintptr_t *)frame)[0];
        record[1] = ((const uintptr_t *)frame)[1];
        return 1;
    }
    mach_vm_size_t got = 0;
    return mach_vm_read_overwrite(mach_task_self(), frame, 16, (mach_vm_address_t)record, &got) ==
               KERN_SUCCESS && got == 16;
}

/* Return addresses of up to `max` callers, starting with the one `frame` returns to. `frame` is
 * a frame pointer of the calling thread (e.g. __builtin_frame_address(0) in a function the guest
 * called, or the guest rbp of a trapped thread). The walk follows strictly ascending frames, at
 * most 8 MiB apart or into the thread's own pthread stack, so it also crosses from a low guest
 * stack into the host frames that switched to it. Returns how many were stored. */
static inline size_t guest_backtrace(uintptr_t frame, uintptr_t *out, size_t max) {
    const uintptr_t top = (uintptr_t)pthread_get_stackaddr_np(pthread_self());
    const uintptr_t bottom = top - pthread_get_stacksize_np(pthread_self());
    size_t n = 0;
    uintptr_t record[2];
    while (n < max && frame && (frame & 7) == 0 &&
           guest_backtrace_frame(frame, bottom, top, record)) {
        if (!record[1]) break;
        out[n++] = record[1];
        if (record[0] <= frame) break;
        /* A large step only into this thread's own stack: the host frames that switched to a
         * low stack (bb_macos_call_on_stack keeps their rbp). */
        if (record[0] - frame > ((uintptr_t)8 << 20) && !(record[0] >= bottom && record[0] < top))
            break;
        frame = record[0];
    }
    return n;
}
#endif
#endif
