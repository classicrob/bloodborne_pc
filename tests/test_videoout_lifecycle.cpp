// SPDX-License-Identifier: GPL-2.0-or-later
// VideoOut port flip lifecycle (core/libraries/videoout/port_lifecycle.h, the functions
// VideoOutDriver uses on macOS), in the interleavings a Close can make with a flip in flight:
// - a flip admitted before Close is dropped before preparing and when publishing, so it neither
//   reads the reset/reopened port's buffers nor decrements the new port's pending count;
// - no flip is admitted to a closed port (the guest gets invalid handle, not success), an EOP flip
//   whose label Close cleared included;
// - no flip of an unregistered buffer is admitted (invalid index, nothing counted), and one
//   unregistered after admission is reported apart from a closed port's;
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
           PortLifecycle::Snapshot(port, g, 0, group, address) ==
                   PortLifecycle::SnapshotResult::Ready &&
               address == 0x1000);
    Expect("  ... published: pending back to 0, nothing released yet",
           PortLifecycle::ApplyStatus(port, g, 0, 7, false, previous) &&
               port.flip_status.flip_pending_num == 0 && port.flip_status.count == 1 &&
               previous == -1);

    // Admitted, then Close and reopen with new buffers, then the old flip goes on.
    u64 old_generation = 0;
    port.buffer_labels[1] = 1; // the EOP's label, written by the GPU
    PortLifecycle::Admit(port, 1, true, old_generation);
    PortLifecycle::ResetOnClose(port);
    Expect("closed port: no flip admitted (invalid handle)",
           PortLifecycle::Admit(port, 0, false, g) == ORBIS_VIDEO_OUT_ERROR_INVALID_HANDLE &&
               port.flip_status.flip_pending_num == 0);
    Expect("  ... an EOP flip whose label Close cleared: invalid handle, not a label assert",
           PortLifecycle::Admit(port, 1, true, g) == ORBIS_VIDEO_OUT_ERROR_INVALID_HANDLE &&
               port.flip_status.gc_queue_num == 0);
    PortLifecycle::Reopen(port);
    Register(port, 1, 0x9000); // the new lifetime's buffer 1
    Expect("flip admitted before Close: not prepared from the reopened port's buffer",
           PortLifecycle::Snapshot(port, old_generation, 1, group, address) ==
               PortLifecycle::SnapshotResult::Closed);
    Expect("  ... and dropped at publication: nothing decremented, nothing released",
           !PortLifecycle::ApplyStatus(port, old_generation, 1, 8, true, previous) &&
               port.flip_status.flip_pending_num == 0 && port.flip_status.gc_queue_num == 0 &&
               port.flip_status.count == 0 && port.prev_index == -1);

    // The reopened port's own flips, through admission, preparation and publication.
    Register(port, 0, 0x8000);
    u64 g1 = 0, g2 = 0;
    const bool admitted = PortLifecycle::Admit(port, 1, false, g1) == ORBIS_OK &&
                          PortLifecycle::Admit(port, 0, false, g2) == ORBIS_OK;
    uintptr_t address1 = 0, address0 = 0;
    const bool prepared =
        PortLifecycle::Snapshot(port, g1, 1, group, address1) ==
            PortLifecycle::SnapshotResult::Ready &&
        PortLifecycle::Snapshot(port, g2, 0, group, address0) ==
            PortLifecycle::SnapshotResult::Ready &&
        address1 == 0x9000 && address0 == 0x8000;
    const bool first = PortLifecycle::ApplyStatus(port, g1, 1, 9, false, previous);
    const int first_previous = previous;
    const bool second = PortLifecycle::ApplyStatus(port, g2, 0, 10, false, previous);
    Expect("reopened port's flips: admitted, prepared from the new buffers",
           admitted && prepared);
    Expect("  ... published in turn, the second releasing the first's buffer",
           first && second && first_previous == -1 && previous == 1 &&
               port.flip_status.flip_pending_num == 0 && port.flip_status.count == 2);

    // Unregistered buffers: refused at admission with nothing counted; unregistered after
    // admission (same lifetime) is told apart from a closed port.
    Expect("unregistered buffer: invalid index, nothing counted",
           PortLifecycle::Admit(port, 5, false, g) == ORBIS_VIDEO_OUT_ERROR_INVALID_INDEX &&
               PortLifecycle::Admit(port, 16, false, g) == ORBIS_VIDEO_OUT_ERROR_INVALID_INDEX &&
               port.flip_status.flip_pending_num == 0);
    u64 g3 = 0;
    PortLifecycle::Admit(port, 1, false, g3);
    port.buffer_slots[1].group_index = -1; // sceVideoOutUnregisterBuffers
    Expect("buffer unregistered after admission: Unregistered, not Closed",
           PortLifecycle::Snapshot(port, g3, 1, group, address) ==
               PortLifecycle::SnapshotResult::Unregistered);
    port.buffer_slots[1].group_index = 0;
    PortLifecycle::ApplyStatus(port, g3, 1, 11, false, previous);

    // Flip queue full is still the guest's error.
    bool full = false;
    for (int i = 0; i < 18 && !full; ++i) {
        full = PortLifecycle::Admit(port, 0, false, g) == ORBIS_VIDEO_OUT_ERROR_FLIP_QUEUE_FULL;
    }
    Expect("more than 16 pending: flip queue full", full);

    std::fprintf(stderr, "%s: %u failures\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
