#pragma once

#ifdef __APPLE__
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" void* runtime_guest_malloc(std::size_t size);
extern "C" void runtime_guest_free(void* pointer);

namespace BbPort {
// Only objects whose addresses are exported to the guest use this allocator.
// Members remain within the same low allocation, including VideoOut labels.
struct GuestAllocated {
    static void* operator new(std::size_t size) {
        if (void* pointer = runtime_guest_malloc(size)) {
            return pointer;
        }
        std::fprintf(stderr, "STOP: low guest-visible GPU allocation failed (%zu bytes)\n", size);
        std::abort();
    }
    static void operator delete(void* pointer) noexcept {
        runtime_guest_free(pointer);
    }
};

// A synchronous guest callback may inspect or modify a host-owned buffer, but
// cannot receive a native malloc/stack address on macOS. Preserve all bytes.
class GuestCopy {
public:
    GuestCopy(const void* source, std::size_t size)
        : pointer(GuestAllocated::operator new(size)), size(size) {
        if (size) std::memcpy(pointer, source, size);
    }
    ~GuestCopy() { runtime_guest_free(pointer); }
    GuestCopy(const GuestCopy&) = delete;
    GuestCopy& operator=(const GuestCopy&) = delete;
    void* Data() const { return pointer; }
    void CopyBack(void* target) const {
        if (size) std::memcpy(target, pointer, size);
    }
private:
    void* pointer;
    std::size_t size;
};
} // namespace BbPort
#endif
