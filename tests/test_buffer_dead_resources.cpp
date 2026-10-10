// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include "gcn_buffer_conformance/gcn_asm.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include <spirv/unified1/spirv.hpp11>
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/specialization.h"

using namespace Shader;

static void Compile(bool live, bool robust, u32 stride) {
    GcnAsm::Code code;
    code.SMovLiteral(GcnAsm::VCC_HI, 0);
    code.SBufferLoad(4, 0, 4, true, 0);
    code.SWaitcntAll();
    if (live) {
        code.VMov(4, 0);
        code.Mubuf(GcnAsm::BUFFER_STORE_DWORD, {.vdata=4, .srsrc=8});
    }
    code.SEndpgm();
    auto source=AmdGpu::Buffer::Placeholder(64);
    source.base_address=0x2000000000ull;
    source.stride=stride;
    source.num_records=stride ? 64/stride : 64;
    auto output=AmdGpu::Buffer::Placeholder(64);
    output.base_address=0x3000000000ull;
    std::array<u32,16> ud{};
    std::memcpy(ud.data()+4, &source, sizeof(source));
    std::memcpy(ud.data()+8, &output, sizeof(output));
    ShaderParams params{.user_data=ud, .code=code.words, .hash=0xF1A7B0F};
    Info info(HwStage::Compute, SwStage::Compute, params);
    RuntimeInfo runtime{};
    runtime.Initialize(HwStage::Compute, SwStage::Compute);
    runtime.props.num_user_data=16;
    runtime.props.num_allocated_vgprs=16;
    runtime.hw.cs.workgroup_size={1,1,1};
    Profile profile{};
    profile.supported_spirv=0x00010500;
    profile.subgroup_size=32;
    profile.support_int64=true;
    profile.emulate_robust_buffer_access=robust;
    Pools pools;
    const auto program=TranslateProgram(code.words, pools, info, runtime, profile);
    assert(!info.buffers.empty());
    assert(info.buffers.front().sharp_fetch.load_mask & 4);
    assert(info.has_readconst==live);
    Backend::Bindings bindings{};
    const auto words=Backend::SPIRV::EmitSPIRV(profile, runtime, program, bindings);
    assert(!words.empty());
    const auto path=std::string("out/mac-inventory/dead-buffer-")+std::to_string(live)+"-"+
                    std::to_string(robust)+"-"+std::to_string(stride)+".spv";
    std::ofstream file(path,std::ios::binary);
    file.write(reinterpret_cast<const char*>(words.data()),words.size()*sizeof(u32));
    assert(file.good());
    std::printf("PASS buffer descriptor hoist live=%u robust=%u stride=%u\n",live,robust,stride);
}

static void Special(BufferType type, bool live) {
    Info info{};
    info.hw_stage=HwStage::Compute;
    info.sw_stage=SwStage::Compute;
    for (u32 i=0;i<2;++i) {
        auto& resource=info.buffers.emplace_back();
        resource.buffer_type=i==0 ? type : BufferType::Guest;
        resource.used_types=IR::Type::U32;
        resource.is_written=i==1;
        auto sharp=AmdGpu::Buffer::Placeholder(64);
        sharp.base_address=0x2000000000ull+i*64;
        std::memcpy(resource.sharp_fetch.immediates.data(),&sharp,sizeof(sharp));
    }
    RuntimeInfo specialization_runtime{};
    specialization_runtime.Initialize(HwStage::Compute,SwStage::Compute);
    Profile specialization_profile{};
    specialization_profile.emulate_robust_buffer_access=true;
    // Internal-only shaders have no ordinary valid-resource bit, but still need
    // a new module when a preceding stage changes their descriptor or push offsets.
    Info internal_info{};
    internal_info.sw_stage=SwStage::Compute;
    internal_info.buffers.push_back(info.buffers[0]);
    internal_info.buffers[0].sharp_fetch = {};
    StageSpecialization first(internal_info,specialization_runtime,specialization_profile,{});
    StageSpecialization shifted(internal_info,specialization_runtime,specialization_profile,
                                {.unified=3,.buffer=2,.user_data=4});
    assert(first.bitset.none() && shifted.bitset.none());
    assert(first!=shifted);
    specialization_profile.emulate_robust_buffer_access=false;
    StageSpecialization native_first(internal_info,specialization_runtime,specialization_profile,{});
    StageSpecialization native_shifted(internal_info,specialization_runtime,specialization_profile,
                                       {.unified=3,.buffer=2,.user_data=4});
    assert(native_first==native_shifted);

    Common::ObjectPool<IR::Inst> pool;
    IR::Block block(pool);
    IR::IREmitter ir(block);
    ir.Prologue();
    if (live) {
        IR::BufferInstInfo flags{};
        const auto value=ir.LoadBufferU32(1,ir.Imm32(0),ir.Imm32(0),flags);
        ir.StoreBufferU32(1,ir.Imm32(1),ir.Imm32(0),value,flags);
    }
    ir.Epilogue();
    IR::Program program(info);
    program.blocks.push_back(&block);
    program.syntax_list.push_back({.data={.block=&block},
                                   .type=IR::AbstractSyntaxNode::Type::Block});
    program.syntax_list.push_back({.type=IR::AbstractSyntaxNode::Type::Return});
    RuntimeInfo runtime{};
    runtime.Initialize(HwStage::Compute,SwStage::Compute);
    runtime.hw.cs.workgroup_size={1,1,1};
    Profile profile{};
    profile.supported_spirv=0x00010500;
    profile.emulate_robust_buffer_access=true;
    Backend::Bindings bindings{};
    const auto words=Backend::SPIRV::EmitSPIRV(profile,runtime,program,bindings);
    u32 length_queries=0;
    for (size_t i=5;i<words.size();) {
        const u32 size=words[i]>>16;
        assert(size);
        length_queries+=(words[i]&0xffff)==u32(spv::Op::OpArrayLength);
        i+=size;
    }
    assert(length_queries==(live ? 2u : 0u));
    const auto path=std::string("out/mac-inventory/dead-special-")+
                    std::to_string(u32(type))+"-"+std::to_string(live)+".spv";
    std::ofstream file(path,std::ios::binary);
    file.write(reinterpret_cast<const char*>(words.data()),words.size()*sizeof(u32));
    assert(file.good());
    std::printf("PASS internal buffer query type=%u live=%u queries=%u\n",
                u32(type),live,length_queries);
}

int main() {
    std::puts("buffer descriptor hoist: production translation and SPIR-V");
    for (bool live : {false,true})
        for (bool robust : {false,true})
            for (u32 stride : {0u,16u}) Compile(live,robust,stride);
    for (auto type : {BufferType::Flatbuf,BufferType::BdaPagetable,BufferType::FaultBuffer,
                      BufferType::GdsBuffer,BufferType::SharedMemory,BufferType::ClipPlanes})
        for (bool live : {false,true}) Special(type,live);
}
