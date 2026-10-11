/* Guest call chain by frame pointers, for diagnostics (macOS).
 *
 * The eboot keeps frame pointers (push rbp; mov rbp, rsp), so from a frame inside the guest's call
 * stack [frame] is the caller's frame and [frame + 8] the return address into it. The walk only
 * reads within the calling thread's stack and stops at the first frame that is not strictly above
 * the previous one, so a frame without a frame pointer ends it early instead of faulting.
 * Header-only: the GPU library and the runtime share it without a host import.
 */
#ifndef BB_GUEST_BACKTRACE_H
#define BB_GUEST_BACKTRACE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __APPLE__
#include <pthread.h>

/* Return addresses of up to `max` callers, starting with the one `frame` returns to. `frame` is
 * a frame pointer on the calling thread's stack (e.g. __builtin_frame_address(0) in a function the
 * guest called, or the guest rbp of a trapped thread). Returns how many were stored. */
static inline size_t guest_backtrace(uintptr_t frame, uintptr_t *out, size_t max) {
    const uintptr_t top = (uintptr_t)pthread_get_stackaddr_np(pthread_self());
    const uintptr_t bottom = top - pthread_get_stacksize_np(pthread_self());
    size_t n = 0;
    while (n < max && frame >= bottom && frame <= top - 16 && (frame & 7) == 0) {
        const uintptr_t next = ((const uintptr_t *)frame)[0];
        const uintptr_t ret = ((const uintptr_t *)frame)[1];
        if (!ret) break;
        out[n++] = ret;
        if (next <= frame) break;
        frame = next;
    }
    return n;
}
#endif
#endif
