// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cstdio>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/recompiler.h"

using namespace Shader;

static void Lower(int scenario) {
    Info info{};
    info.pgm_hash = 0xf640;
    IR::Program program{info};
    Pools pools;
    auto* block = pools.block_pool.Create(pools.inst_pool);
    program.blocks.push_back(block);
    IR::IREmitter ir{*block};
    const auto a = ir.Imm64(1.0);
    const auto b = ir.Imm64(2.0);
    switch (scenario) {
    case 0:
        (void)ir.FPMul(ir.Imm32(1.0f), ir.Imm32(2.0f));
        (void)ir.IAdd(ir.Imm64(u64(1)), ir.Imm64(u64(2)));
        break;
    case 1: (void)ir.FPMul(a, b); break;
    case 2: (void)ir.FPLessThan(a, b); break;
    case 3: (void)ir.FPConvert(32, a); break;
    case 4: (void)ir.PackDouble2x32(ir.CompositeConstruct(ir.Imm32(0u), ir.Imm32(0u))); break;
    case 5: (void)ir.UnpackDouble2x32(a); break;
    default: assert(false);
    }
    Optimization::LowerFp64ToFp32(program);
    if (scenario == 0) {
        unsigned fp32 = 0, u64 = 0;
        for (const auto& inst : block->Instructions()) {
            fp32 += inst.GetOpcode() == IR::Opcode::FPMul32;
            u64 += inst.GetOpcode() == IR::Opcode::IAdd64;
        }
        assert(fp32 == 1 && u64 == 1);
    }
}

int main() {
    Lower(0);
    std::puts("FP32/U64 control PASS");
#ifdef __APPLE__
    for (int scenario = 1; scenario <= 5; ++scenario) {
        int output[2]; assert(pipe(output) == 0);
        std::fflush(nullptr);
        const pid_t child = fork(); assert(child >= 0);
        if (!child) {
            close(output[0]);
            assert(dup2(output[1], STDERR_FILENO) >= 0);
            close(output[1]);
            Lower(scenario);
            _Exit(0);
        }
        close(output[1]);
        std::string message;
        char bytes[1024];
        ssize_t count;
        while ((count = read(output[0], bytes, sizeof(bytes))) > 0) message.append(bytes, count);
        assert(count == 0); close(output[0]);
        int status; assert(waitpid(child, &status, 0) == child);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 23);
        assert(message.find("Apple shader FP64 requires exact support") != std::string::npos);
        assert(message.find("shader=0xf640") != std::string::npos);
        std::printf("FP64 result/operand/conversion/pack rejection case %d PASS\n", scenario);
    }
#endif
}
