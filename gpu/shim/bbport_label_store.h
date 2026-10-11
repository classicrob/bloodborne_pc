// bbport (macOS): a GPU label stored through the guest's own mapping (no backing write path) as
// one release store, so the guest sees the old or the new value, never part of each. memcpy of a
// variable size gives no such guarantee, and a guest that frees a label's node as soon as it reads
// the final value would otherwise race a second partial store into the freed node.
// The backing path does the same (runtime_memory_write_backing).
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace BbLabelStore {

/// `address` is aligned to `num_bytes` (CheckFenceAddress); 2, 4 or 8 bytes.
inline void StoreOnce(void* address, std::uint64_t value, std::uint32_t num_bytes) {
    switch (num_bytes) {
    case 8:
        __atomic_store_n(static_cast<std::uint64_t*>(address), value, __ATOMIC_RELEASE);
        return;
    case 4:
        __atomic_store_n(static_cast<std::uint32_t*>(address), static_cast<std::uint32_t>(value),
                         __ATOMIC_RELEASE);
        return;
    case 2:
        __atomic_store_n(static_cast<std::uint16_t*>(address), static_cast<std::uint16_t>(value),
                         __ATOMIC_RELEASE);
        return;
    default:
        std::fprintf(stderr, "STOP: label store of %u bytes at %p (2, 4 or 8 expected)\n",
                     num_bytes, address);
        std::abort();
    }
}

} // namespace BbLabelStore
