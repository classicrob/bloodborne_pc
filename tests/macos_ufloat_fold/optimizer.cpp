// SPDX-License-Identifier: GPL-2.0-or-later
// Packed unsigned floats (10_11_11) survive ConstantPropagationPass: Unpack(Pack(x)) quantizes x
// and Pack(Unpack(raw)) may canonicalize a NaN, so neither pair may fold to its input (the GPU
// conformance group `ufloat` cannot see this: its stores and loads go through memory).
#include <cassert>
#include <cstdio>
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/ir_passes.h"

int main() {
    using Shader::IR::Opcode;
    Common::ObjectPool<Shader::IR::Inst> pool;
    Shader::IR::Block block(pool);
    Shader::IR::IREmitter ir(block);
    // Unpack(Pack(x)): 1.0001 is not an 11-bit float, so the round trip changes it.
    const auto x = ir.CompositeConstruct(ir.Imm32(1.0001f), ir.Imm32(0.3f), ir.Imm32(70000.0f));
    const auto packed = ir.Pack10_11_11(AmdGpu::NumberFormat::Float, x);
    const auto unpacked = ir.Unpack10_11_11(AmdGpu::NumberFormat::Float, packed);
    // Pack(Unpack(raw)) of a loaded word (a NaN payload may change).
    const auto raw = ir.LoadBufferU32(1, ir.Imm32(0), ir.Imm32(0), {});
    const auto widened = ir.Unpack10_11_11(AmdGpu::NumberFormat::Float, Shader::IR::U32{raw});
    const auto narrowed = ir.Pack10_11_11(AmdGpu::NumberFormat::Float, widened);
    ir.StoreBufferF32(3, ir.Imm32(0), ir.Imm32(0), unpacked, {});
    ir.StoreBufferU32(1, ir.Imm32(0), ir.Imm32(0), narrowed, {});
    Shader::IR::BlockList blocks{&block};
    Shader::Optimization::ConstantPropagationPass(blocks);
    const bool kept = packed.Inst()->GetOpcode() == Opcode::PackUfloat10_11_11 &&
                      unpacked.Inst()->GetOpcode() == Opcode::UnpackUfloat10_11_11 &&
                      widened.Inst()->GetOpcode() == Opcode::UnpackUfloat10_11_11 &&
                      narrowed.Inst()->GetOpcode() == Opcode::PackUfloat10_11_11;
    std::printf("Unpack(Pack(x)) and Pack(Unpack(raw)) of 10_11_11 survive optimization: %s\n",
                kept ? "PASS" : "FAIL (an inverse fold removed a conversion)");
    return kept ? 0 : 1;
}
