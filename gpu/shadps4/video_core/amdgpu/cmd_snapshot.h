// SPDX-License-Identifier: GPL-2.0-or-later
// bbport (macOS): command memory of one graphics submission, copied when the guest submits it.
//
// On the PS4 the command processor fetches a submitted buffer long before the guest may reuse it.
// Here the GPU command thread parses later, and production runs tore on command memory the guest
// changed after submitting it ("Unimplemented PM4 type 0", "Wrong PM4 type 1"). The parser
// therefore reads the copies taken at submit: the top-level DCB and CCB and every indirect buffer
// they reach. REWIND ("refetch from memory") switches parsing to live guest memory. A
// WRITE_DATA/DMA_DATA into command memory not yet parsed would be seen by the PS4 command processor
// but not in a copy, so it is a hard error. See docs/macos/command-snapshots.md.
#pragma once

#include <memory>
#include <span>
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

    u64 submit_number{};
    u64 copied_bytes{};
    u64 copy_ns{};
    bool done{}; ///< GPU command thread: the submission is parsed

private:
    std::vector<u32> words;
    std::vector<Region> regions;
    bool has_ccb{};
};

} // namespace AmdGpu
