// bbport (macOS diagnostic, BB_LABEL_CHECK=1): did the content at an end-of-pipe label change
// between its EOP being parsed and its write?
//
// Context: the guest frees per-frame objects that hold EOP labels (48-byte heap blocks, label at
// offset 0) once the submitDone throttle releases it. A label written after that lands in freed
// memory, where the allocator's free-list link at offset 0 receives the label value (Guest fault
// 0x263b8e7, docs/history/SESSION_2026-09-30.md). So the value at a label address is sampled when
// its EOP is parsed, and again just before the write. A change in between that is not one of our
// own completed writes is reported (LABEL-CHECK): something other than this EOP changed the
// memory, e.g. the guest freeing or reusing the object, or resetting the label.
//
// Supporting evidence only. A change need not be a guest free (other writers exist), and no
// change does not prove the lifetime is right (a free that leaves the value equal, ABA, or a change
// after the second sample are invisible). `pending` is the time from the parse sample to the write
// check, not the time of any free. The counts (checked, changed, unreadable, unmatched, mixed)
// are printed every 4096 checks so that zero changes can be weighed against what was checked.
//
// Guest memory is read only through non-faulting Mach reads; an unreadable sample is counted and
// never compared. History is kept per 8-byte slot. A slot whose pending or completed writes
// differ in address or width (a 32-bit and a 64-bit EOP on one label) is counted as mixed and
// not compared.
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_map>
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif

namespace BbLabelCheck {

inline bool Enabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_LABEL_CHECK");
        return env && env[0] == '1';
    }();
    return enabled;
}

/// Reads guest memory without faulting; false if any byte is unreadable.
inline bool SafeRead(std::uint64_t address, void* out, std::size_t size) {
#ifdef __APPLE__
    mach_vm_size_t got = 0;
    return mach_vm_read_overwrite(mach_task_self(), address, size,
                                  reinterpret_cast<mach_vm_address_t>(out), &got) ==
               KERN_SUCCESS &&
           got == size;
#else
    (void)address;
    (void)out;
    (void)size;
    return false;
#endif
}

struct Pending {
    std::uint64_t address;
    std::uint32_t num_bytes;
    std::uint64_t generation; ///< completed writes to the slot before the sample
    std::uint64_t observed;
    bool readable;
    std::chrono::steady_clock::time_point parsed;
};

struct Slot {
    std::deque<Pending> pending;
    std::uint64_t generation = 0;   ///< our completed writes to this slot
    std::uint64_t last_value = 0;   ///< what the latest one wrote
    std::uint64_t last_address = 0; ///< and where, with its width
    std::uint32_t last_bytes = 0;
    std::uint32_t sampling = 0; ///< NoteParsed calls between their generation read and push
    bool mixed = false;
};

struct Counts {
    std::atomic<std::uint64_t> checked{0}, changed{0}, unreadable{0}, unmatched{0}, mixed{0};
};

inline std::mutex mutex;
inline std::unordered_map<std::uint64_t, Slot> slots; ///< by 8-byte slot address
inline Counts counts;

inline std::uint64_t Mask(std::uint32_t num_bytes) {
    return num_bytes >= 8 ? ~std::uint64_t{0} : (std::uint64_t{1} << (8 * num_bytes)) - 1;
}

/// Reads the label's bytes (little endian) without faulting.
inline bool Sample(std::uint64_t address, std::uint32_t num_bytes, std::uint64_t& value) {
    value = 0;
    return SafeRead(address, &value, num_bytes < 8 ? num_bytes : 8);
}

inline void NoteShape(Slot& slot, std::uint64_t address, std::uint32_t num_bytes) {
    for (const auto& entry : slot.pending) {
        if (entry.address != address || entry.num_bytes != num_bytes) {
            slot.mixed = true;
        }
    }
    if (slot.last_bytes && (slot.last_address != address || slot.last_bytes != num_bytes)) {
        slot.mixed = true;
    }
}

/// When the EOP is parsed, before it is handed to the recording thread (which may write it at
/// once). The generation is taken before the sample: a write completing in between is seen as
/// ours by the check.
inline void NoteParsed(std::uint64_t address, std::uint32_t num_bytes) {
    std::uint64_t generation;
    {
        std::scoped_lock lk{mutex};
        Slot& slot = slots[address & ~std::uint64_t{7}];
        generation = slot.generation;
        ++slot.sampling; // the slot (and its generation) outlives a write completing meanwhile
    }
    const auto parsed = std::chrono::steady_clock::now();
    std::uint64_t value;
    const bool readable = Sample(address, num_bytes, value);
    std::scoped_lock lk{mutex};
    Slot& slot = slots[address & ~std::uint64_t{7}];
    --slot.sampling;
    NoteShape(slot, address, num_bytes);
    slot.pending.push_back({address, num_bytes, generation, value, readable, parsed});
}

/// Just before the label is written. Returns true if a change was reported.
inline bool BeforeWrite(std::uint64_t address, std::uint32_t num_bytes) {
    std::uint64_t current;
    const bool readable = Sample(address, num_bytes, current);
    Pending entry;
    bool ours = false, mixed = false;
    {
        std::scoped_lock lk{mutex};
        const auto it = slots.find(address & ~std::uint64_t{7});
        if (it == slots.end() || it->second.pending.empty()) {
            counts.unmatched.fetch_add(1, std::memory_order_relaxed);
            return false; // parsed before the check was on, or written without a parse note
        }
        Slot& slot = it->second;
        entry = slot.pending.front();
        slot.pending.pop_front();
        mixed = slot.mixed || entry.address != address || entry.num_bytes != num_bytes;
        // One of our writes completed after this EOP's sample, and its value is what is there.
        ours = slot.generation > entry.generation && current == (slot.last_value & Mask(num_bytes));
    }
    const std::uint64_t n_checked = counts.checked.fetch_add(1, std::memory_order_relaxed) + 1;
    bool reported = false;
    if (!readable || !entry.readable) {
        counts.unreadable.fetch_add(1, std::memory_order_relaxed);
    } else if (mixed) {
        counts.mixed.fetch_add(1, std::memory_order_relaxed);
    } else if (current != entry.observed && !ours) {
        reported = true;
        const std::uint64_t n = counts.changed.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 16 || (n & 1023) == 0) {
            const double ms = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - entry.parsed)
                                  .count();
            std::uint64_t block[6]{};
            const bool block_readable =
                SafeRead(address & ~std::uint64_t{15}, block, sizeof(block));
            std::fprintf(stderr,
                         "LABEL-CHECK #%llu: label %#llx (%u bytes) changed between its EOP's "
                         "parse and write, not by our writes: %#llx when parsed, %#llx now "
                         "(pending %.1f ms since the parse sample); block %s %#llx %#llx %#llx "
                         "%#llx %#llx %#llx\n",
                         static_cast<unsigned long long>(n),
                         static_cast<unsigned long long>(address), num_bytes,
                         static_cast<unsigned long long>(entry.observed),
                         static_cast<unsigned long long>(current), ms,
                         block_readable ? "" : "(unreadable)",
                         static_cast<unsigned long long>(block[0]),
                         static_cast<unsigned long long>(block[1]),
                         static_cast<unsigned long long>(block[2]),
                         static_cast<unsigned long long>(block[3]),
                         static_cast<unsigned long long>(block[4]),
                         static_cast<unsigned long long>(block[5]));
        }
    }
    if ((n_checked & 4095) == 0) {
        std::fprintf(stderr,
                     "LABEL-CHECK summary: %llu writes checked, %llu changed, %llu unreadable, "
                     "%llu unmatched, %llu mixed widths\n",
                     static_cast<unsigned long long>(n_checked),
                     static_cast<unsigned long long>(counts.changed.load()),
                     static_cast<unsigned long long>(counts.unreadable.load()),
                     static_cast<unsigned long long>(counts.unmatched.load()),
                     static_cast<unsigned long long>(counts.mixed.load()));
    }
    return reported;
}

/// After the label was written with `data`: later checks of EOPs sampled before this point
/// accept this value as ours.
inline void AfterWrite(std::uint64_t address, std::uint64_t data, std::uint32_t num_bytes) {
    std::scoped_lock lk{mutex};
    const auto it = slots.find(address & ~std::uint64_t{7});
    if (it == slots.end()) {
        return;
    }
    Slot& slot = it->second;
    NoteShape(slot, address, num_bytes);
    ++slot.generation;
    slot.last_value = data & Mask(num_bytes);
    slot.last_address = address;
    slot.last_bytes = num_bytes;
    if (slot.pending.empty() && slot.sampling == 0) {
        slots.erase(it); // no EOP sampled before this write is left to check
    }
}

} // namespace BbLabelCheck
