// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cstdio>
#include "shader_recompiler/frontend/decode.h"
#include "shader_recompiler/frontend/translate/translate.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/profile.h"

int main() {
    {
        const u32 word=(47u<<25)|(2u<<17)|(1u<<9)|256u;
        Shader::Gcn::GcnCodeSlice slice(&word,&word+1);
        Shader::Gcn::GcnDecodeContext decoder;
        const auto instruction=decoder.decodeInstruction(slice);
        assert(instruction.opcode==Shader::Gcn::Opcode::V_CVT_PKRTZ_F16_F32);
        Shader::Info info{};
        info.hw_stage=Shader::HwStage::Compute;
        info.sw_stage=Shader::SwStage::Compute;
        Shader::RuntimeInfo runtime{};
        runtime.Initialize(Shader::HwStage::Compute,Shader::SwStage::Compute);
        runtime.props.num_allocated_vgprs=4;
        Shader::Profile profile{};
        Common::ObjectPool<Shader::IR::Inst> pool;
        Shader::IR::Block block(pool);
        Shader::Gcn::Translator translator(info,runtime,profile);
        translator.EmitPrologue(&block);
        translator.V_CVT_PKRTZ_F16_F32(instruction);
        unsigned count=0;
        for(auto& inst:block.Instructions()) {
            if(inst.GetOpcode()==Shader::IR::Opcode::PackHalf2x16) {
                assert(inst.Flags<bool>());
                ++count;
            }
        }
        assert(count==1);
        std::puts("Decoded VOP2 PKRTZ translates to flagged RTZ pack: PASS");
    }
    Common::ObjectPool<Shader::IR::Inst> pool;
    Shader::IR::Block block(pool);
    Shader::IR::IREmitter ir(block);
    const auto input=ir.CompositeConstruct(ir.Imm32(1.0001f),ir.Imm32(-1.0001f));
    const auto packed=ir.PackHalf2x16Rtz(input);
    const auto unpacked=ir.Unpack2x16(AmdGpu::NumberFormat::Float,packed);
    const auto repacked=ir.PackHalf2x16Rtz(unpacked);
    ir.StoreBufferU32(1,ir.Imm32(0),ir.Imm32(0),repacked,{});
    Shader::IR::BlockList blocks{&block};
    Shader::Optimization::ConstantPropagationPass(blocks);
    assert(packed.Inst()->GetOpcode()==Shader::IR::Opcode::PackHalf2x16);
    assert(unpacked.Inst()->GetOpcode()==Shader::IR::Opcode::UnpackHalf2x16);
    assert(repacked.Inst()->GetOpcode()==Shader::IR::Opcode::PackHalf2x16);
    assert(packed.Inst()->Flags<bool>() && repacked.Inst()->Flags<bool>());
    std::puts("RTZ pack/unpack round trips survive optimization: PASS");
}
