// SPDX-License-Identifier: GPL-2.0-or-later
// bbport (macOS): a VideoOut port's flip lifecycle, shared by VideoOutDriver and its test.
//
// A flip is admitted (counted pending, stamped with the port's generation) only to an open port.
// Close resets the port and bumps the generation under port_mutex, in one step. A flip admitted
// before a Close is then dropped wherever it is: before its frame is prepared (it would read the
// reset or reopened port's buffers) and when it would be published (it would decrement the new
// port's pending count, or release a buffer of the new lifetime).
#pragma once
#include <cstring>
#include <mutex>
#include "core/libraries/kernel/time.h"
#include "core/libraries/videoout/driver.h"
#include "core/libraries/videoout/videoout_error.h"
#include "core/libraries/videoout/video_out.h"

namespace Libraries::VideoOut::PortLifecycle {

/// Admits a flip: ORBIS_OK and its generation, or the guest's error (closed port: invalid handle;
/// more than 16 flips pending for a buffer: flip queue full).
inline s32 Admit(VideoOutPort& port, s32 index, bool is_eop, u64& generation) {
    std::unique_lock lock{port.port_mutex};
    if (!port.is_open) {
        return ORBIS_VIDEO_OUT_ERROR_INVALID_HANDLE;
    }
    if (index != -1 && port.flip_status.flip_pending_num > 16) {
        return ORBIS_VIDEO_OUT_ERROR_FLIP_QUEUE_FULL;
    }
    if (is_eop) {
        ++port.flip_status.gc_queue_num;
    }
    ++port.flip_status.flip_pending_num; // integral GPU and CPU pending flips counter
    port.flip_status.submit_tsc = Libraries::Kernel::sceKernelReadTsc();
    generation = port.generation;
    return ORBIS_OK;
}

/// Before preparing the frame: false if the port was closed since the flip was admitted (the flip
/// is dropped). Else copies what preparing needs (none for a blank flip, index -1).
inline bool Snapshot(VideoOutPort& port, u64 generation, s32 index, BufferAttributeGroup& group,
                     uintptr_t& address) {
    std::unique_lock lock{port.port_mutex};
    if (port.generation != generation) {
        return false;
    }
    if (index != -1) {
        const auto& buffer = port.buffer_slots[index];
        if (buffer.group_index < 0) {
            return false;
        }
        group = port.groups[buffer.group_index];
        address = buffer.address_left;
    }
    return true;
}

/// Close's reset of everything a flip reads or writes, in one step with the generation bump.
inline void ResetOnClose(VideoOutPort& port) {
    std::unique_lock lock{port.port_mutex};
    ++port.generation;
    port.is_open = false;
    port.flip_rate = 0;
    port.prev_index = -1;
    std::memset(port.buffer_labels.data(), 0, sizeof(port.buffer_labels));
    std::memset(port.groups.data(), 0, sizeof(port.groups));
    std::memset(&port.vblank_status, 0, sizeof(port.vblank_status));
    port.flip_status = FlipStatus{};
    std::memset(port.buffer_slots.data(), 0, sizeof(port.buffer_slots));
    for (auto& buffer : port.buffer_slots) {
        buffer.group_index = -1;
    }
}

inline void Reopen(VideoOutPort& port) {
    std::unique_lock lock{port.port_mutex};
    port.is_open = true;
}

/// A flip's status update when published: false if the port moved on since it was admitted (the
/// flip is dropped: nothing decremented, no buffer released). Else `previous` is the buffer the
/// flip releases (-1: none).
inline bool ApplyStatus(VideoOutPort& port, u64 generation, s32 index, s64 flip_arg, bool eop,
                        int& previous) {
    std::unique_lock lock{port.port_mutex};
    if (port.generation != generation) {
        return false;
    }
    auto& flip_status = port.flip_status;
    flip_status.count++;
    flip_status.process_time = Libraries::Kernel::sceKernelGetProcessTime();
    flip_status.tsc = Libraries::Kernel::sceKernelReadTsc();
    flip_status.flip_arg = flip_arg;
    flip_status.current_buffer = index;
    if (eop) {
        --flip_status.gc_queue_num;
    }
    --flip_status.flip_pending_num;
    previous = port.prev_index;
    port.prev_index = index;
    return true;
}

} // namespace Libraries::VideoOut::PortLifecycle
