// SPDX-License-Identifier: GPL-2.0-or-later
// FlattenExtendedUserdataPass reads the components of every ReadConst base (a (low, high) SGPR
// pair, S_LOAD_DWORD) and every sharp-source ReadConstBuffer V# (four SGPRs, S_BUFFER_LOAD_DWORD).
// Any other shape, e.g. a phi of whole pointers after divergent control flow, must stop naming
// the shader (exit 23 with the pass's message) before anything reads it as components.
// - Positive: a bindless V# loaded with s_buffer_load_dwordx4 and used by buffer loads, through
//   the production recompiler (TranslateProgram); this is the shape a ReadConst-only (two SGPR)
//   check would wrongly reject.
// - Negative: a ReadConst on a whole-pointer phi, and a sharp-source ReadConstBuffer whose handle
//   is two SGPRs.
#include <array>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include "gcn_buffer_conformance/gcn_asm.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/recompiler.h"

using namespace Shader;

namespace {
u32 failures = 0;
void Expect(const char* what, bool ok) {
    std::fprintf(stderr, "%-70s %s\n", what, ok ? "ok" : "FAIL");
    failures += ok ? 0 : 1;
}

struct ChildResult {
    int status;         // exit code, or 128 + signal
    std::string output; // what the child wrote to stderr
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

// The child stopped through this pass's shape check (not an incidental assertion elsewhere).
bool StoppedWith(const ChildResult& r, const char* message) {
    return r.status == 23 && r.output.find(message) != std::string::npos;
}

// s_buffer_load_dwordx4 s[12:15], s[4:7], 0; buffer_load_dword v4, s[12:15];
// buffer_store_dword v4, s[8:11]. The constant buffer at s[4:7] holds the inner V#.
void CompileBindlessBuffer() {
    // Guest-like memory below 2^44 (a V# holds a 44-bit address) for the SRT walker to read.
    void* table = mmap(reinterpret_cast<void*>(0x300000000ull), 0x4000, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(table != MAP_FAILED && reinterpret_cast<uintptr_t>(table) < (1ull << 44));
    auto inner = AmdGpu::Buffer::Placeholder(64);
    inner.base_address = 0x2000000000ull;
    std::memcpy(table, &inner, sizeof(inner));
    auto cbuf = AmdGpu::Buffer::Placeholder(64);
    cbuf.base_address = reinterpret_cast<uintptr_t>(table);
    auto output = AmdGpu::Buffer::Placeholder(64);
    output.base_address = 0x3000000000ull;
    std::array<u32, 16> ud{};
    std::memcpy(ud.data() + 4, &cbuf, sizeof(cbuf));
    std::memcpy(ud.data() + 8, &output, sizeof(output));

    GcnAsm::Code code;
    code.SMovLiteral(GcnAsm::VCC_HI, 0);
    code.SBufferLoad(4, 12, 4, true, 0);
    code.SWaitcntAll();
    code.Mubuf(GcnAsm::BUFFER_LOAD_DWORD, {.vdata = 4, .srsrc = 12});
    code.SWaitcntAll();
    code.Mubuf(GcnAsm::BUFFER_STORE_DWORD, {.vdata = 4, .srsrc = 8});
    code.SEndpgm();

    ShaderParams params{.user_data = ud, .code = code.words, .hash = 0xB1D1E55};
    Info info(HwStage::Compute, SwStage::Compute, params);
    RuntimeInfo runtime{};
    runtime.Initialize(HwStage::Compute, SwStage::Compute);
    runtime.props.num_user_data = 16;
    runtime.props.num_allocated_vgprs = 16;
    runtime.hw.cs.workgroup_size = {1, 1, 1};
    Profile profile{};
    profile.supported_spirv = 0x00010500;
    profile.subgroup_size = 32;
    profile.support_int64 = true;
    Pools pools;
    (void)TranslateProgram(code.words, pools, info, runtime, profile);
    // The loaded V# went through the flattened user data (a sharp-source ReadConstBuffer).
    assert(info.srt_info.flattened_bufsize_dw > ud.size());
}

void CompileComputedBuffer(bool unsupported, bool audit) {
    setenv("BB_SHADER_SHARP_AUDIT", audit ? "1" : "0", 1);
    setenv("BB_GPU_LOG", "warning", 1);
    auto input = AmdGpu::Buffer::Placeholder(64);
    input.base_address = 0x2000000000ull;
    auto output = AmdGpu::Buffer::Placeholder(64);
    output.base_address = 0x3000000000ull;
    std::array<u32, 16> ud{};
    std::memcpy(ud.data() + 4, &input, sizeof(input));
    std::memcpy(ud.data() + 8, &output, sizeof(output));
    GcnAsm::Code code;
    code.SMovLiteral(GcnAsm::VCC_HI, 0);
    if (unsupported) {
        code.Sop2(1, 6, 6, 4); // s_sub_u32: computed num_records, not a supported post-op.
    } else {
        code.Sop2(16, 5, 5, GcnAsm::LITERAL); // s_or_b32: supported stride patch.
        code.words.push_back(16u << 16);
    }
    code.SBufferLoad(1, 12, 4, true, 0);
    code.SWaitcntAll();
    code.VMov(4, 12);
    code.Mubuf(GcnAsm::BUFFER_STORE_DWORD, {.vdata = 4, .srsrc = 8});
    code.SEndpgm();
    ShaderParams params{.user_data = ud, .code = code.words, .hash = 0xc0de5a9e};
    Info info(HwStage::Compute, SwStage::Compute, params);
    RuntimeInfo runtime{};
    runtime.Initialize(HwStage::Compute, SwStage::Compute);
    runtime.props.num_user_data = 16;
    runtime.props.num_allocated_vgprs = 16;
    runtime.hw.cs.workgroup_size = {1, 1, 1};
    Profile profile{};
    profile.supported_spirv = 0x00010500;
    profile.subgroup_size = 32;
    profile.support_int64 = true;
    Pools pools;
    (void)TranslateProgram(code.words, pools, info, runtime, profile);
    bool patched = false;
    for (const auto& buffer : info.buffers) {
        if (buffer.post_op != SharpFetchPostOp::BitwiseOrDw1WithImm) continue;
        const auto sharp = buffer.GetSharp(info);
        assert(sharp.base_address == input.base_address && sharp.stride == 16);
        assert(sharp.num_records == input.num_records);
        patched = true;
    }
    assert(patched);
}

enum class Shape { ReadConstPhi, BufferPair };

// One block with a single ReadConst / sharp-source ReadConstBuffer of a malformed base.
void FlattenMalformed(Shape shape) {
    std::array<u32, 16> user_data{};
    const std::array<u32, 1> code{0xBF810000u}; // s_endpgm
    ShaderParams params{.user_data = user_data, .code = code, .hash = 0xBA5E5A9E};
    Info info(HwStage::Compute, SwStage::Compute, params);
    Common::ObjectPool<IR::Inst> pool;
    IR::Block block(pool);
    IR::IREmitter ir(block);
    ir.Prologue();
    if (shape == Shape::ReadConstPhi) {
        // A whole-pointer phi (its incoming edges are irrelevant: the check looks at the base).
        IR::Inst* phi = &*block.PrependNewInst(block.end(), IR::Opcode::Phi, {});
        phi->SetFlags(IR::Type::U32x2);
        (void)ir.ReadConst(IR::Value{phi}, ir.Imm32(0));
    } else {
        IR::BufferInstInfo flags{};
        flags.sharp_source.Assign(1);
        const auto ud = [&](u32 reg) { return ir.GetUserData(IR::ScalarReg(reg)); };
        (void)ir.ReadConstBuffer(ir.CompositeConstruct(ud(4), ud(5)), ir.Imm32(0), ir.Imm32(0),
                                 flags);
    }
    ir.Epilogue();
    IR::Program program(info);
    program.blocks.push_back(&block);
    program.post_order_blocks.push_back(&block);
    Optimization::FlattenExtendedUserdataPass(program);
}
} // namespace

int main() {
    const auto computed = InChild([] { CompileComputedBuffer(false, true); });
    Expect("computed sharp audit logs a supported stride patch before rewriting it",
           computed.status == 0 && computed.output.find(
               "SHARP-COMPUTED shader=0xc0de5a9e kind=buffer dword=1 producer=BitwiseOr32") !=
               std::string::npos);
    const auto unsupported = InChild([] { CompileComputedBuffer(true, true); });
    Expect("computed num_records is reported and rejected, never replaced by an operand",
           StoppedWith(unsupported, "Shader 0xc0de5a9e sharp dword 2 has unsupported producer") &&
           unsupported.output.find("SHARP-COMPUTED shader=0xc0de5a9e kind=buffer dword=2") !=
               std::string::npos);
    const auto audit_off = InChild([] { CompileComputedBuffer(false, false); });
    Expect("computed sharp audit is silent when disabled", audit_off.status == 0 &&
           audit_off.output.find("SHARP-COMPUTED") == std::string::npos);
    Expect("bindless V# (s_buffer_load_dwordx4) compiles through the production recompiler",
           InChild(CompileBindlessBuffer).status == 0);
    Expect("ReadConst on a whole-pointer phi stops (exit 23)",
           StoppedWith(InChild([] { FlattenMalformed(Shape::ReadConstPhi); }),
                       "ReadConst base of shader 0xba5e5a9e is Phi, not CompositeConstructU32x2"));
    Expect("sharp-source ReadConstBuffer on a two-SGPR handle stops (exit 23)",
           StoppedWith(InChild([] { FlattenMalformed(Shape::BufferPair); }),
                       "ReadConstBuffer base of shader 0xba5e5a9e is CompositeConstructU32x2, "
                       "not CompositeConstructU32x4"));
    std::fprintf(stderr, "%s: %u failures\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
