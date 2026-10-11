// SPDX-License-Identifier: GPL-2.0-or-later
// IT_STRMOUT_BUFFER_UPDATE and register WAIT_REG_MEM on macOS.
// - Bloodborne's Gnm stream-out setup (eboot 0x1077d80, called before each stream-out draw) emits
//   {0xC0043400, buffer << 8, 0, 0, offset, 0}: the buffer's offset from the packet, no
//   filled-size store. That form decodes; every other form stops (exit 23).
// - A register wait on the graphics queue reads registers only that queue's packets set, so one
//   unmet when reached is never met: it stops instead of spinning. CP_STRMOUT_CNTL's
//   OFFSET_UPDATE_DONE, set when SO_VGTSTREAMOUT_FLUSH is parsed, meets the Gnm flush wait.
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include "video_core/amdgpu/pm4_cmds.h"
#include "video_core/amdgpu/regs.h"

using namespace AmdGpu;

namespace {
u32 failures = 0;
void Expect(const char* what, bool ok) {
    std::fprintf(stderr, "%-72s %s\n", what, ok ? "ok" : "FAIL");
    failures += ok ? 0 : 1;
}

struct ChildResult {
    int status;
    std::string output;
};

// Runs `body` in a child, capturing its stderr (echoed here as well).
template <typename F>
ChildResult InChild(F body) {
    std::fflush(stderr);
    int fds[2];
    if (pipe(fds) != 0) {
        std::perror("pipe");
        std::exit(2);
    }
    const pid_t child = fork();
    if (child < 0) {
        std::perror("fork");
        std::exit(2);
    }
    if (child == 0) {
        close(fds[0]);
        dup2(fds[1], STDERR_FILENO);
        body();
        std::_Exit(0);
    }
    close(fds[1]);
    ChildResult result{};
    char buf[4096];
    for (ssize_t n; (n = read(fds[0], buf, sizeof(buf))) != 0;) {
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) break;
        result.output.append(buf, size_t(n));
        std::fwrite(buf, 1, size_t(n), stderr);
    }
    close(fds[0]);
    int status = 0;
    pid_t waited;
    while ((waited = waitpid(child, &status, 0)) < 0 && errno == EINTR) {
    }
    if (waited != child) {
        std::perror("waitpid");
        std::exit(2);
    }
    result.status = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    return result;
}

bool StoppedWith(const ChildResult& r, const char* message) {
    return r.status == 23 && r.output.find(message) != std::string::npos;
}

// The packet Bloodborne's builder writes (eboot 0x1077d80), or the general form's fields.
std::array<u32, 6> Strmout(u32 buffer, u32 offset, u32 update_memory = 0, u32 source = 0) {
    return {0xC0043400u, update_memory | source << 1 | (buffer & 3) << 8, 0, 0, offset, 0};
}

std::array<u32, 7> WaitReg(u32 reg, u32 function, u32 ref, u32 mask) {
    return {0xC0053C00u, function, reg, 0, ref, mask, 10};
}
} // namespace

int main() {
    static_assert(sizeof(PM4CmdStrmoutBufferUpdate) == 6 * sizeof(u32));
    static_assert(sizeof(PM4CmdWaitRegMem) == 7 * sizeof(u32));

    bool decodes = true;
    for (u32 buffer = 0; buffer < 4; ++buffer) {
        const auto words = Strmout(buffer, 0x40 + buffer);
        const auto& packet = *reinterpret_cast<const PM4CmdStrmoutBufferUpdate*>(words.data());
        decodes &= packet.header.opcode.Value() == PM4ItOpcode::StrmoutBufferUpdate &&
                   packet.header.NumWords() == 5 && packet.SetsOffsetFromPacket() &&
                   packet.buffer_select.Value() == buffer && packet.buffer_offset == 0x40 + buffer;
    }
    Expect("Gnm's form (offset from the packet) decodes, buffers 0-3", decodes);
    Expect("Gnm's form passes the implemented-form check", InChild([] {
               const auto words = Strmout(2, 0);
               reinterpret_cast<const PM4CmdStrmoutBufferUpdate*>(words.data())
                   ->AssertImplementedForm();
           }).status == 0);
    Expect("filled-size store (update_memory 1) stops naming the form",
           StoppedWith(InChild([] {
                           const auto words = Strmout(1, 0, 1, 3);
                           reinterpret_cast<const PM4CmdStrmoutBufferUpdate*>(words.data())
                               ->AssertImplementedForm();
                       }),
                       "IT_STRMOUT_BUFFER_UPDATE form not implemented: update_memory 1, "
                       "source_select 3, buffer 1"));
    Expect("offset from memory (source_select 2) stops", StoppedWith(InChild([] {
               const auto words = Strmout(0, 0, 0, 2);
               reinterpret_cast<const PM4CmdStrmoutBufferUpdate*>(words.data())
                   ->AssertImplementedForm();
           }),
                                                                       "source_select 2"));

    // The Gnm flush idiom: CP_STRMOUT_CNTL = 0, EVENT_WRITE SO_VGTSTREAMOUT_FLUSH (sets
    // OFFSET_UPDATE_DONE when parsed), WAIT_REG_MEM CP_STRMOUT_CNTL & 1 == 1.
    static Regs regs{};
    constexpr u32 CpStrmoutCntl = 0xC03F;
    regs.cp_strmout_cntl.offset_update_done = 1;
    Expect("flush wait on CP_STRMOUT_CNTL after the flush event passes", InChild([] {
               const auto words = WaitReg(CpStrmoutCntl, 3 /*Equal*/, 1, 1);
               reinterpret_cast<const PM4CmdWaitRegMem*>(words.data())
                   ->AssertRegisterWaitMet(regs.reg_array);
           }).status == 0);
    Expect("a register wait unmet when reached stops instead of spinning",
           StoppedWith(InChild([] {
                           regs.cp_strmout_cntl.offset_update_done = 0;
                           const auto words = WaitReg(CpStrmoutCntl, 3, 1, 1);
                           reinterpret_cast<const PM4CmdWaitRegMem*>(words.data())
                               ->AssertRegisterWaitMet(regs.reg_array);
                       }),
                       "WAIT_REG_MEM on register 0xc03f can never be met"));
    Expect("a register index outside the register file stops", StoppedWith(InChild([] {
               const auto words = WaitReg(0xE000, 3, 1, 1);
               reinterpret_cast<const PM4CmdWaitRegMem*>(words.data())
                   ->AssertRegisterInRange(Regs::NumRegs);
           }),
                                                                             "outside the register file"));
    Expect("a memory wait is not a register wait", InChild([] {
               auto words = WaitReg(0xE000, 3, 1, 1);
               words[1] |= 1u << 4; // mem_space = Memory
               reinterpret_cast<const PM4CmdWaitRegMem*>(words.data())
                   ->AssertRegisterInRange(Regs::NumRegs);
           }).status == 0);
    std::fprintf(stderr, "%s: %u failures\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
