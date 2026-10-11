// SPDX-License-Identifier: GPL-2.0-or-later
// Fence/label writes (EVENT_WRITE_EOP, EVENT_WRITE_EOS, RELEASE_MEM) land at the guest address
// the packet names, aligned to the write size and below 2^48. Anything else would land elsewhere
// than on the PS4 command processor, so on Apple it stops (exit 23) naming the packet.
#include <cstdio>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>
#include "video_core/amdgpu/pm4_cmds.h"

// The clock sources the fence writers can use; not exercised here.
namespace Libraries::Kernel {
u64 PS4_SYSV_ABI sceKernelGetTscFrequency() {
    return 1;
}
u64 PS4_SYSV_ABI sceKernelReadTsc() {
    return 0;
}
} // namespace Libraries::Kernel
namespace Libraries::GnmDriver {
u32 PS4_SYSV_ABI sceGnmGetGpuCoreClockFrequency() {
    return 1;
}
} // namespace Libraries::GnmDriver

namespace {
using namespace AmdGpu;
u32 failures = 0;
void Expect(const char* what, bool ok) {
    std::fprintf(stderr, "%-62s %s\n", what, ok ? "ok" : "FAIL");
    failures += ok ? 0 : 1;
}

alignas(16) u8 labels[64];

/// A zeroed packet (the packet structs have no default constructor).
template <typename T>
T& Zeroed() {
    alignas(8) static u32 raw[16];
    std::memset(raw, 0, sizeof(raw));
    static_assert(sizeof(T) <= sizeof(raw));
    return *reinterpret_cast<T*>(raw);
}

PM4CmdEventWriteEop Eop(u64 address, DataSelect select) {
    auto& eop = Zeroed<PM4CmdEventWriteEop>();
    eop.address_lo = u32(address);
    eop.address_hi.Assign(u32(address >> 32) & 0xFFFF);
    eop.data_sel.Assign(select);
    eop.data_lo = 0x11223344;
    eop.data_hi = 0x55667788;
    return eop;
}

// Runs `body` in a child; true if it stopped with the GPU assertion exit code.
template <typename F>
bool Stops(F body) {
    std::fflush(stderr);
    const pid_t child = fork();
    if (child == 0) {
        body();
        std::_Exit(0);
    }
    int status = 0;
    waitpid(child, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 23;
}

void Write(void* address, u64 data, u32 bytes) {
    std::memcpy(address, &data, bytes);
}
} // namespace

int main() {
    const u64 base = reinterpret_cast<u64>(labels);

    std::memset(labels, 0, sizeof(labels));
    Eop(base + 8, DataSelect::Data64).SignalFence(Write, [] {});
    u64 q;
    std::memcpy(&q, labels + 8, 8);
    Expect("EOP 64-bit to an 8-aligned label", q == 0x5566778811223344ull);
    Eop(base + 4, DataSelect::Data32Low).SignalFence(Write, [] {});
    u32 d;
    std::memcpy(&d, labels + 4, 4);
    Expect("EOP 32-bit to a 4-aligned label", d == 0x11223344u);
    Expect("EOP 64-bit to a 4-aligned label stops", Stops([&] {
               Eop(base + 4, DataSelect::Data64).SignalFence(Write, [] {});
           }));
    Expect("EOP 32-bit to a 2-aligned label stops", Stops([&] {
               Eop(base + 2, DataSelect::Data32Low).SignalFence(Write, [] {});
           }));

    auto& eos = Zeroed<PM4CmdEventWriteEos>();
    eos.address_lo = u32(base + 12);
    eos.address_hi.Assign(u32(base >> 32) & 0xFFFF);
    eos.command.Assign(PM4CmdEventWriteEos::Command::SignalFence);
    eos.data = 0xCAFE;
    eos.SignalFence(Write);
    std::memcpy(&d, labels + 12, 4);
    Expect("EOS to a 4-aligned label", d == 0xCAFEu);
    Expect("EOS to a 1-aligned label stops", Stops([&] {
               auto bad = eos;
               bad.address_lo = u32(base + 13);
               bad.SignalFence(Write);
           }));

    auto& release = Zeroed<PM4CmdReleaseMem>();
    release.address_lo = u32(base + 16);
    release.address_hi = u32(base >> 32);
    release.data_sel.Assign(DataSelect::Data64);
    release.data_lo = 1;
    release.data_hi = 2;
    release.int_sel.Assign(InterruptSelect::None);
    release.SignalFence([] {}, [](VAddr, u16, u16) {});
    std::memcpy(&q, labels + 16, 8);
    Expect("RELEASE_MEM 64-bit to an 8-aligned label", q == 0x0000000200000001ull);
    Expect("RELEASE_MEM with address bits above 47 stops", Stops([&] {
               auto bad = release;
               bad.address_hi |= 0x00010000u;
               bad.SignalFence([] {}, [](VAddr, u16, u16) {});
           }));

    std::fprintf(stderr, "%s: %u failures\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
