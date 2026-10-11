// SPDX-License-Identifier: GPL-2.0-or-later
// bbport (macOS): command memory of one graphics submission, copied when the guest submits it.
//
// On the PS4 the command processor fetches a submitted buffer long before the guest may reuse it.
// Here the GPU command thread parses later, and production runs tore on command memory the guest
// changed after submitting it ("Unimplemented PM4 type 0", "Wrong PM4 type 1"). The parser
// therefore reads the copies taken at submit: the top-level DCB and CCB and every indirect buffer
// they reach. REWIND ("refetch from memory") switches parsing to live guest memory. Copies wait for
// WRITE_DATA/DMA_DATA writes parsed before the submit that have not landed yet (StreamWrites); one
// parsed after the submit into command memory not yet parsed would be seen by the PS4 command
// processor but not in a copy, so it is a hard error. See docs/macos/command-snapshots.md.
#pragma once

#include <memory>
#include <span>
#include <string>
#include <vector>
#include "common/types.h"

namespace AmdGpu {

class CmdSnapshot {
public:
    struct Region {
        const u32* guest;  ///< guest address of the buffer
        u32 size_dw;       ///< dwords as submitted / as in the IB packet
        u32 copy_offset;   ///< first dword in `words`
        bool constant;     ///< constant engine (CCB, IndirectBufferConst)
        bool copied;       ///< false: unreadable at submit (a valid stream never executes it)
        u32 parsed_dw = 0; ///< GPU command thread: dwords consumed so far
    };

    /// Copies `dcb`, `ccb` and every indirect buffer they reference.
    [[nodiscard]] static std::shared_ptr<CmdSnapshot> Take(std::span<const u32> dcb,
                                                           std::span<const u32> ccb);

    [[nodiscard]] Region& Dcb() {
        return regions[0];
    }
    [[nodiscard]] Region* Ccb() {
        return has_ccb ? &regions[1] : nullptr;
    }
    [[nodiscard]] std::span<const u32> Words(const Region& region) const {
        return {words.data() + region.copy_offset, region.size_dw};
    }
    /// The region captured for an indirect buffer packet, or nullptr.
    [[nodiscard]] Region* Find(const u32* guest, u32 size_dw, bool constant);

    /// Whether [address, address + size) overlaps command memory of this submission the GPU
    /// command thread has not consumed yet.
    [[nodiscard]] bool OverlapsUnparsed(VAddr address, u64 size) const;

    /// BB_CMD_SNAPSHOT_CHECK diagnostic: compares a region with live guest memory and logs guest
    /// writes made after submit. Returns the number of differing dwords.
    u32 ReportGuestRewrites(const Region& region) const;

    /// Diagnostic, at submit: logs regions of this submission that partly overlap command memory
    /// submitted in the last 64 submissions, except a buffer the same thread submits again at the
    /// same start (whatever its length). A command ring takes far longer to wrap, so any other
    /// overlap means two command buffers were written into the same memory. Returns the number of
    /// overlaps.
    u32 ReportRecentOverlaps();

    u64 submit_number{};
    u64 copied_bytes{};
    u64 copy_ns{};
    u32 writes_waited{}; ///< StreamWrites this copy waited for (included in copy_ns)
    bool done{}; ///< GPU command thread: the submission is parsed

private:
    std::vector<u32> words;
    std::vector<Region> regions;
    bool has_ccb{};
};

/// WRITE_DATA / DMA_DATA memory writes the GPU command thread has parsed but that have not
/// reached guest memory yet: they run later, in order, on the draw recording thread. The PS4
/// command processor executes them before it fetches anything submitted after them, and games
/// patch command chunks that way. A copy taken at submit (CmdSnapshot::Take) therefore waits for
/// the ones overlapping what it copies. See docs/macos/command-snapshots.md.
class StreamWrites {
public:
    /// GPU command thread, before handing the write on. Writes land in the order added.
    static void Add(VAddr address, u64 size);
    /// Whoever executed the oldest added write, once it ran. `through_gpu`: it was recorded as a
    /// GPU copy or fill, which reaches guest memory only when the GPU runs it.
    static void Landed(bool through_gpu);
    /// Waits until no added write overlaps [address, address + size); returns how many it waited
    /// for. A write it waited for that went through the GPU is a hard error (not implemented).
    static u32 WaitLanded(VAddr address, u64 size);
};

/// The latest calls of the sceGnm* command builders (which guest thread wrote which dwords), so a
/// malformed packet can be traced to the writes around it.
class BuilderTrace {
public:
    struct Call {
        const char* builder;
        const u32* cmdbuf;
        u32 size;   ///< dwords the guest reserved
        u32 thread; ///< small per-thread number, in order of first call
        u64 seq;
        u64 tsc; ///< x86 time stamp counter at the call (the HLE trace's clock); 0 elsewhere
        u32 depth;              ///< return addresses in `chain`
        uintptr_t chain[10];    ///< callers (guest, then host HLE), innermost first
    };
    /// Guest thread, on entry to a builder. `frame`: the builder's frame pointer
    /// (__builtin_frame_address(0)), from which the guest call chain is walked; 0 for none.
    static void Note(const char* builder, const u32* cmdbuf, u32 size, uintptr_t frame = 0);
    /// The name the given trace thread number had when it first called a builder, and its
    /// pthread_self() (which the guest entry probe also logs), as "name pthread 0x...".
    [[nodiscard]] static std::string ThreadName(u32 thread);
    /// The calling thread's number in the trace.
    [[nodiscard]] static u32 ThreadNumber();
    /// Recorded calls (oldest first) whose dwords overlap [begin, end), plus the `last` latest.
    [[nodiscard]] static std::vector<Call> Around(const u32* begin, const u32* end, u32 last);
    /// Logs Around() for a malformed packet at `at`.
    static void Report(const u32* at);
    /// With BB_CMD_SNAPSHOT_CHECK=1, Note() logs CMD-INTERLEAVE when two threads advance one
    /// command stream (A writes, B writes on from there, A writes on from B); how many so far.
    [[nodiscard]] static u64 Interleaves();
};

} // namespace AmdGpu
