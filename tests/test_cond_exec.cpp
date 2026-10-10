// SPDX-License-Identifier: GPL-2.0-or-later
// IT_COND_EXEC tests the 32-bit value at BOOL_ADDR: zero skips EXEC_COUNT dwords, anything else
// executes them. PM4CmdCondExec::Address() was typed bool*, so the parser read one byte through a
// bool (undefined for values other than 0/1; 0x100 read as false), skipped dwords the PS4 executes
// and lost packet alignment.
#include <cstdio>
#include <cstring>
#include <span>
#include "video_core/amdgpu/pm4_cmds.h"

int main() {
    using AmdGpu::PM4CmdCondExec;
    unsigned failures = 0;
    alignas(8) static volatile u32 predicates[6] = {0, 1, 2, 0x100, 0x10000, 0xFFFFFFFFu};
    for (u32 i = 0; i < 6; ++i) {
        const u64 address = reinterpret_cast<u64>(&predicates[i]);
        u32 packet[4]{};
        packet[1] = u32(address);                 // BOOL_ADDR_LO (dword aligned)
        packet[2] = u32(address >> 32) & 0xFFFF;  // BOOL_ADDR_HI
        packet[3] = 7;                            // EXEC_COUNT
        static_assert(sizeof(PM4CmdCondExec) == sizeof(packet));
        const auto& cond = *reinterpret_cast<const PM4CmdCondExec*>(packet);
        const bool skip = cond.Predicate() == 0;
        const bool want_skip = predicates[i] == 0;
        const bool ok = skip == want_skip && cond.Address() == &predicates[i] &&
                        cond.exec_count.Value() == 7;
        std::fprintf(stderr, "predicate %#010x -> %s %s\n", u32(predicates[i]),
                     skip ? "skip" : "execute", ok ? "ok" : "FAIL");
        failures += ok ? 0 : 1;
    }
    std::fprintf(stderr, "%s: %u failures\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
