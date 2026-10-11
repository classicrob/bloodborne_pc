// SPDX-License-Identifier: GPL-2.0-or-later
// VideoOut port flip lifecycle (core/libraries/videoout/port_lifecycle.h, the functions
// VideoOutDriver uses on macOS), in the interleavings a Close can make with a flip in flight:
// - a flip admitted before Close is dropped before preparing and when publishing, so it neither
//   reads the reset/reopened port's buffers nor decrements the new port's pending count;
// - no flip is admitted to a closed port (the guest gets invalid handle, not success);
// - after a reopen, the new port's flips are counted and published normally.
#include <cstdio>
#include "core/libraries/videoout/port_lifecycle.h"

using namespace Libraries::VideoOut;

namespace {
unsigned failures = 0;
void Expect(const char* what, bool ok) {
    std::fprintf(stderr, "%-72s %s\n", what, ok ? "ok" : "FAIL");
    failures += ok ? 0 : 1;
}

void Register(VideoOutPort& port, s32 index, uintptr_t address) {
    port.groups[0].is_occupied = true;
    port.buffer_slots[index].group_index = 0;
    port.buffer_slots[index].address_left = address;
}
} // namespace

int main() {
    static VideoOutPort port{};
    PortLifecycle::Reopen(port);
    Register(port, 0, 0x1000);
    Register(port, 1, 0x2000);

    // In order: admit, prepare, publish.
    u64 g = 0;
    BufferAttributeGroup group{};
    uintptr_t address = 0;
    int previous = -2;
    Expect("open port: flip admitted",
           PortLifecycle::Admit(port, 0, false, g) == ORBIS_OK &&
               port.flip_status.flip_pending_num == 1);
    Expect("  ... prepared from its buffer",
           PortLifecycle::Snapshot(port, g, 0, group, address) && address == 0x1000);
    Expect("  ... published: pending back to 0, nothing released yet",
           PortLifecycle::ApplyStatus(port, g, 0, 7, false, previous) &&
               port.flip_status.flip_pending_num == 0 && port.flip_status.count == 1 &&
               previous == -1);

    // Admitted, then Close and reopen with new buffers, then the old flip goes on.
    u64 old_generation = 0;
    PortLifecycle::Admit(port, 1, true, old_generation);
    PortLifecycle::ResetOnClose(port);
    Expect("closed port: no flip admitted (invalid handle)",
           PortLifecycle::Admit(port, 0, false, g) == ORBIS_VIDEO_OUT_ERROR_INVALID_HANDLE &&
               port.flip_status.flip_pending_num == 0);
    PortLifecycle::Reopen(port);
    Register(port, 1, 0x9000); // the new lifetime's buffer 1
    Expect("flip admitted before Close: not prepared from the reopened port's buffer",
           !PortLifecycle::Snapshot(port, old_generation, 1, group, address));
    Expect("  ... and dropped at publication: nothing decremented, nothing released",
           !PortLifecycle::ApplyStatus(port, old_generation, 1, 8, true, previous) &&
               port.flip_status.flip_pending_num == 0 && port.flip_status.gc_queue_num == 0 &&
               port.flip_status.count == 0 && port.prev_index == -1);

    // The reopened port's own flips.
    u64 g1 = 0, g2 = 0;
    PortLifecycle::Admit(port, 1, false, g1);
    PortLifecycle::Admit(port, 0, false, g2);
    const bool first = PortLifecycle::ApplyStatus(port, g1, 1, 9, false, previous);
    const int first_previous = previous;
    const bool second = PortLifecycle::ApplyStatus(port, g2, 0, 10, false, previous);
    Expect("reopened port's flips published in turn, the second releasing the first's buffer",
           first && second && first_previous == -1 && previous == 1 &&
               port.flip_status.flip_pending_num == 0 && port.flip_status.count == 2);

    // Flip queue full is still the guest's error.
    bool full = false;
    for (int i = 0; i < 18 && !full; ++i) {
        full = PortLifecycle::Admit(port, 0, false, g) == ORBIS_VIDEO_OUT_ERROR_FLIP_QUEUE_FULL;
    }
    Expect("more than 16 pending: flip queue full", full);

    std::fprintf(stderr, "%s: %u failures\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
