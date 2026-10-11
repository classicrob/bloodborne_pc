// SPDX-License-Identifier: GPL-2.0-or-later
// CmdSnapshot (macOS submit-time command copies): the parser must see what the guest submitted,
// including nested and constant indirect buffers, regardless of later guest writes; unreadable IB
// targets are recorded, not fatal; unparsed ranges and guest rewrites are reported exactly.
// Stream writes parsed before a submit but not landed yet are waited for (StreamWrites).
#include <chrono>
#include <csetjmp>
#include <csignal>
#include <cstdio>
#include <thread>
#include <vector>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
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

    // A WRITE_DATA parsed before the submit patches a command chunk the submission calls; the
    // recording thread lands it later. The CP would fetch the chunk after the write.
    {
        std::vector<u32> chunk(4, 0u); // stale: zeros, as a fresh chunk
        std::vector<u32> sub;
        AppendNop(sub, 0x9999);
        AppendIb(sub, PM4ItOpcode::IndirectBuffer, chunk.data(), u32(chunk.size()));
        StreamWrites::Add(reinterpret_cast<VAddr>(chunk.data()), chunk.size() * sizeof(u32));
        std::thread recorder([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            const u32 words[] = {Header(PM4ItOpcode::Nop, 1), 0xAAAA, Header(PM4ItOpcode::Nop, 1),
                                 0xBBBB};
            std::copy(std::begin(words), std::end(words), chunk.begin());
            StreamWrites::Landed(false);
        });
        auto patched = CmdSnapshot::Take(sub, {});
        recorder.join();
        auto* chunk_r = patched->Find(chunk.data(), u32(chunk.size()), false);
        Expect("pending write into a called chunk waited for",
               patched->writes_waited == 1);
        Expect("chunk copied with the write applied",
               chunk_r && chunk_r->copied && patched->Words(*chunk_r).size() == 4 &&
                   patched->Words(*chunk_r)[1] == 0xAAAA && patched->Words(*chunk_r)[3] == 0xBBBB);
    }

    // The submitted buffer itself is patched: the walk sees the IB packet the write puts there.
    {
        std::vector<u32> target;
        AppendNop(target, 0xCCCC);
        std::vector<u32> sub{Header(PM4ItOpcode::Nop, 3), 0, 0, 0}; // placeholder for an IB
        StreamWrites::Add(reinterpret_cast<VAddr>(sub.data()), sub.size() * sizeof(u32));
        std::thread recorder([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            std::vector<u32> ib;
            AppendIb(ib, PM4ItOpcode::IndirectBuffer, target.data(), u32(target.size()));
            std::copy(ib.begin(), ib.end(), sub.begin());
            StreamWrites::Landed(false);
        });
        auto patched = CmdSnapshot::Take(sub, {});
        recorder.join();
        Expect("patched DCB copied after the write",
               patched->Words(patched->Dcb())[0] == Header(PM4ItOpcode::IndirectBuffer, 3));
        Expect("IB written by the pending write is captured",
               patched->Find(target.data(), u32(target.size()), false) != nullptr);
    }

    // A pending write elsewhere does not hold the submit up.
    {
        std::vector<u32> elsewhere(4, 0u), sub;
        AppendNop(sub, 0xDDDD);
        StreamWrites::Add(reinterpret_cast<VAddr>(elsewhere.data()), 16);
        auto unaffected = CmdSnapshot::Take(sub, {});
        Expect("non-overlapping pending write not waited for", unaffected->writes_waited == 0);
        StreamWrites::Landed(false);
        Expect("nothing pending after it landed",
               StreamWrites::WaitLanded(reinterpret_cast<VAddr>(elsewhere.data()), 16) == 0);
    }

    // A DMA_DATA into the chunk recorded as a GPU copy cannot be in the copy: a hard error.
    {
        std::fflush(stderr);
        const pid_t child = fork();
        if (child == 0) {
            std::vector<u32> chunk(2, 0u), sub;
            AppendIb(sub, PM4ItOpcode::IndirectBuffer, chunk.data(), u32(chunk.size()));
            StreamWrites::Add(reinterpret_cast<VAddr>(chunk.data()), 8);
            std::thread recorder([] {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                StreamWrites::Landed(true);
            });
            auto never = CmdSnapshot::Take(sub, {});
            recorder.join();
            std::_Exit(0);
        }
        int status = 0;
        waitpid(child, &status, 0);
        Expect("GPU-side DMA into submitted command memory stops (exit 23)",
               WIFEXITED(status) && WEXITSTATUS(status) == 23);
    }

    // Builder calls are traced: the ones overlapping a window, plus the latest, oldest first.
    {
        static u32 stream[256];
        BuilderTrace::Note("sceGnmSetCsShader", stream + 100, 25);
        BuilderTrace::Note("sceGnmDrawIndexAuto", stream + 10, 7);
        std::thread([] { BuilderTrace::Note("sceGnmSetVsShader", stream + 140, 29); }).join();
        const auto near = BuilderTrace::Around(stream + 120, stream + 130, 0);
        Expect("trace: call overlapping the window found, other excluded",
               near.size() == 1 && near[0].cmdbuf == stream + 100 && near[0].size == 25);
        const auto latest = BuilderTrace::Around(stream, stream, 2);
        Expect("trace: latest calls kept in order, threads told apart",
               latest.size() == 2 && latest[0].cmdbuf == stream + 10 &&
                   latest[1].cmdbuf == stream + 140 && latest[0].thread != latest[1].thread);
        BuilderTrace::Report(stream + 106);
    }

    // Command memory submitted twice within a few submissions, partly overlapping: reported.
    {
        static u32 ring[64];
        for (u32 i = 0; i < 64; i += 2) {
            ring[i] = Header(PM4ItOpcode::Nop, 1);
        }
        const auto submit = [](u32 first, u32 count, u64 number) {
            auto snapshot = CmdSnapshot::Take(std::span<const u32>{ring + first, count}, {});
            snapshot->submit_number = number;
            return snapshot->ReportRecentOverlaps();
        };
        Expect("overlap: first submission of the range clean", submit(0, 16, 1000) == 0);
        Expect("overlap: same buffer called again not reported", submit(0, 16, 1001) == 0);
        Expect("overlap: same start, other length not reported", submit(0, 7, 1001) == 0);
        u32 cross = 0;
        std::thread([&] { cross = submit(0, 7, 1001); }).join();
        Expect("overlap: same start from another thread reported", cross == 3);
        Expect("overlap: partial overlap reported", submit(10, 16, 1002) == 2);
        Expect("overlap: adjacent buffer clean", submit(26, 8, 1003) == 0);
        Expect("overlap: older than 64 submissions forgotten", submit(4, 8, 1100) == 0);
    }

    std::fprintf(stderr, "%s: %u failures\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
