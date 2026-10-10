// SPDX-License-Identifier: GPL-2.0-or-later
// CmdSnapshot (macOS submit-time command copies): the parser must see what the guest submitted,
// including nested and constant indirect buffers, regardless of later guest writes; unreadable IB
// targets are recorded, not fatal; unparsed ranges and guest rewrites are reported exactly.
#include <csetjmp>
#include <csignal>
#include <cstdio>
#include <vector>
#include <sys/mman.h>
#include "video_core/amdgpu/cmd_snapshot.h"
#include "video_core/amdgpu/pm4_cmds.h"

extern "C" {
__thread sigjmp_buf* runtime_fault_recover = nullptr;
}

namespace {
u32 failures = 0;
void Expect(const char* what, bool ok) {
    std::fprintf(stderr, "%-62s %s\n", what, ok ? "ok" : "FAIL");
    failures += ok ? 0 : 1;
}

// The probe resumes a speculative read's recovery point on a fault; the test does the same.
void OnFault(int, siginfo_t*, void*) {
    if (runtime_fault_recover) {
        siglongjmp(*runtime_fault_recover, 1);
    }
    std::_Exit(3);
}

using namespace AmdGpu;

u32 Header(PM4ItOpcode op, u32 body_words) {
    const PM4Type3Header header{op, body_words - 1};
    u32 raw;
    std::memcpy(&raw, &header, sizeof(raw));
    return raw;
}

// IT_INDIRECT_BUFFER(_CONST): header, base lo, base hi, size.
void AppendIb(std::vector<u32>& stream, PM4ItOpcode op, const u32* target, u32 size_dw) {
    const auto address = reinterpret_cast<u64>(target);
    stream.insert(stream.end(),
                  {Header(op, 3), u32(address), u32(address >> 32), size_dw});
}

void AppendNop(std::vector<u32>& stream, u32 marker) {
    stream.insert(stream.end(), {Header(PM4ItOpcode::Nop, 1), marker});
}
} // namespace

int main() {
    struct sigaction action{};
    action.sa_sigaction = OnFault;
    action.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &action, nullptr);
    sigaction(SIGBUS, &action, nullptr);

    // Guest memory: an inner IB, an outer IB calling it twice, a constant IB, a DCB, a CCB.
    std::vector<u32> inner, outer, const_ib, dcb, ccb;
    AppendNop(inner, 0x1111);
    AppendNop(outer, 0x2222);
    AppendIb(outer, PM4ItOpcode::IndirectBuffer, inner.data(), u32(inner.size()));
    AppendIb(outer, PM4ItOpcode::IndirectBuffer, inner.data(), u32(inner.size()));
    AppendNop(const_ib, 0x3333);
    AppendNop(dcb, 0x4444);
    AppendIb(dcb, PM4ItOpcode::IndirectBuffer, outer.data(), u32(outer.size()));
    dcb.push_back(0x80000000u); // a type-2 padding dword
    AppendNop(ccb, 0x5555);
    AppendIb(ccb, PM4ItOpcode::IndirectBufferConst, const_ib.data(), u32(const_ib.size()));

    // An IB named in the stream whose memory is unmapped (as inside a skipped COND_EXEC region).
    void* hole = mmap(nullptr, 1 << 14, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    AppendIb(dcb, PM4ItOpcode::IndirectBuffer, static_cast<const u32*>(hole), 8);

    auto snap = CmdSnapshot::Take(dcb, ccb);
    auto& top = snap->Dcb();
    Expect("top-level DCB copied verbatim",
           std::ranges::equal(snap->Words(top), std::span<const u32>{dcb}));
    Expect("CCB copied verbatim", snap->Ccb() && std::ranges::equal(snap->Words(*snap->Ccb()),
                                                                     std::span<const u32>{ccb}));
    auto* outer_r = snap->Find(outer.data(), u32(outer.size()), false);
    auto* inner_r = snap->Find(inner.data(), u32(inner.size()), false);
    auto* const_r = snap->Find(const_ib.data(), u32(const_ib.size()), true);
    auto* hole_r = snap->Find(static_cast<const u32*>(hole), 8, false);
    Expect("nested IB found and copied", outer_r && outer_r->copied &&
                                             std::ranges::equal(snap->Words(*outer_r),
                                                                std::span<const u32>{outer}));
    Expect("second-level IB found and copied", inner_r && inner_r->copied &&
                                                   snap->Words(*inner_r)[1] == 0x1111);
    Expect("constant IB from the CCB found and copied",
           const_r && const_r->copied && snap->Words(*const_r)[1] == 0x3333);
    Expect("a graphics IB is not found as a constant IB",
           snap->Find(inner.data(), u32(inner.size()), true) == nullptr);
    Expect("unreadable IB target recorded as not copied", hole_r && !hole_r->copied);
    Expect("IB called twice copied once",
           snap->copied_bytes == (dcb.size() + ccb.size() + outer.size() + inner.size() +
                                  const_ib.size()) * sizeof(u32));

    // The guest rewrites everything after submit: the copies do not change.
    for (auto* buffer : {&dcb, &outer, &inner, &const_ib, &ccb}) {
        for (auto& word : *buffer) {
            word = 0;
        }
    }
    Expect("copies unaffected by later guest writes",
           snap->Words(top)[1] == 0x4444 && snap->Words(*inner_r)[1] == 0x1111 &&
               snap->Words(*const_r)[1] == 0x3333);
    Expect("guest rewrite of the DCB reported (diagnostic)",
           snap->ReportGuestRewrites(top) == dcb.size());

    // Unparsed command memory, as the parser advances.
    const auto top_address = reinterpret_cast<VAddr>(dcb.data());
    Expect("whole DCB unparsed before parsing", snap->OverlapsUnparsed(top_address, 4));
    top.parsed_dw = 2;
    Expect("parsed part no longer protected", !snap->OverlapsUnparsed(top_address, 8));
    Expect("rest still protected", snap->OverlapsUnparsed(top_address + 8, 4));
    for (auto* region : {&top, outer_r, inner_r, const_r, snap->Ccb()}) {
        region->parsed_dw = region->size_dw;
    }
    Expect("nothing protected once parsed",
           !snap->OverlapsUnparsed(top_address, dcb.size() * 4) &&
               !snap->OverlapsUnparsed(reinterpret_cast<VAddr>(inner.data()), 8));
    Expect("unreadable region never protected",
           !snap->OverlapsUnparsed(reinterpret_cast<VAddr>(hole), 32));

    // A chained IB is a jump: words after it in the same buffer are never walked.
    std::vector<u32> chain_target, chained, decoy;
    AppendNop(chain_target, 0x7777);
    AppendNop(decoy, 0x8888);
    AppendIb(chained, PM4ItOpcode::IndirectBuffer, chain_target.data(), u32(chain_target.size()));
    chained[3] |= 1u << 20; // CHAIN
    AppendIb(chained, PM4ItOpcode::IndirectBuffer, decoy.data(), u32(decoy.size())); // dead tail
    chained.insert(chained.end(), {0u, 0u}); // zeroed chunk tail
    auto chain_snap = CmdSnapshot::Take(chained, {});
    Expect("chain target captured",
           chain_snap->Find(chain_target.data(), u32(chain_target.size()), false) != nullptr);
    Expect("IB after a chain (dead tail) not walked",
           chain_snap->Find(decoy.data(), u32(decoy.size()), false) == nullptr);

    // A truncated packet ends the walk without reading past the buffer.
    std::vector<u32> truncated{Header(PM4ItOpcode::IndirectBuffer, 3), 0};
    auto partial = CmdSnapshot::Take(truncated, {});
    Expect("truncated stream copied, no IB followed",
           partial->copied_bytes == truncated.size() * sizeof(u32));

    std::fprintf(stderr, "%s: %u failures\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
