// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include "gcn_buffer_conformance/gcn_asm.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/specialization.h"

using namespace Shader;

static void Compile(bool pulling, bool typed, u32 instance_vgpr) {
    GcnAsm::Code fetch;
    // s_load_dwordx4 s[4:7], s[2:3], 0
    fetch.words.push_back(0xC0000000u | (2u << 22) | (4u << 15) | (1u << 9) | (1u << 8));
    fetch.SWaitcntAll();
    const GcnAsm::Code::BufFields fields{.idxen=true, .vaddr=instance_vgpr, .vdata=4, .srsrc=4};
    if (typed) fetch.Mtbuf(GcnAsm::TBUFFER_LOAD_FORMAT_XYZW, 14, 7, fields);
    else fetch.Mubuf(GcnAsm::BUFFER_LOAD_FORMAT_XYZW, fields);
    fetch.SWaitcntAll();
    fetch.Sop1(32, 0, 0); // s_setpc_b64 s[0:1]

    GcnAsm::Code main;
    main.SMovLiteral(GcnAsm::VCC_HI, 0);
    main.Sop1(33, 0, 0); // s_swappc_b64 s[0:1], s[0:1]
    main.Mubuf(GcnAsm::BUFFER_STORE_DWORDX4,
               {.idxen=true, .vaddr=0, .vdata=4, .srsrc=8});
    main.SEndpgm();

    auto source = AmdGpu::Buffer::Placeholder(64);
    source.base_address = 0x2000000000ull;
    source.stride = 16;
    source.num_records = 4;
    source.data_format = u32(AmdGpu::DataFormat::Format32_32_32_32);
    source.num_format = u32(AmdGpu::NumberFormat::Float);
    auto destination = source;
    destination.base_address = 0x3000000000ull;
    destination.num_records = 16;
    std::array<u32, 16> ud{};
    const auto* fetch_pointer = fetch.words.data();
    const auto* descriptor_pointer = &source;
    std::memcpy(ud.data(), &fetch_pointer, sizeof(fetch_pointer));
    std::memcpy(ud.data()+2, &descriptor_pointer, sizeof(descriptor_pointer));
    std::memcpy(ud.data()+8, &destination, sizeof(destination));
    ShaderParams params{.user_data=ud, .code=main.words, .hash=0x12345678};
    Info info(HwStage::Vertex, SwStage::Vertex, params);
    RuntimeInfo runtime{};
    runtime.Initialize(HwStage::Vertex, SwStage::Vertex);
    runtime.props.num_user_data=16;
    runtime.props.num_allocated_vgprs=16;
    runtime.props.num_input_vgprs=3;
    runtime.sw.vs.step_rate_0=2;
    runtime.sw.vs.step_rate_1=3;
    Profile profile{};
    profile.supported_spirv=0x00010500;
    profile.subgroup_size=32;
    profile.support_int64=true;
    profile.support_int8=true;
    profile.support_int16=true;
    profile.needs_buffer_offsets=true;
    profile.emulate_null_descriptors=true;
    profile.emulate_robust_buffer_access=true;
    profile.emulate_vertex_fetch=pulling;
    Pools pools;
    const auto program = TranslateProgram(main.words, pools, info, runtime, profile);
    bool source_binding=false;
    for (const auto& buffer : info.buffers) {
        if (!buffer.IsSpecial() && buffer.GetSharp(info).base_address==source.base_address)
            source_binding=true;
    }
    assert(source_binding==pulling);
    assert(info.loads.GetAny(IR::Attribute::Param0)==!pulling);
    Backend::Bindings binding{};
    const auto words=Backend::SPIRV::EmitSPIRV(profile, runtime, program, binding);
    assert(!words.empty());
    const auto filename=std::string("out/mac-inventory/vertex-fetch-")+std::to_string(pulling)+
                        "-"+std::to_string(typed)+"-"+std::to_string(instance_vgpr)+".spv";
    std::ofstream file(filename, std::ios::binary);
    file.write(reinterpret_cast<const char*>(words.data()), words.size()*sizeof(u32));
    assert(file.good());
    auto original=Gcn::ParseFetchShader(info);
    assert(original);
    fetch.words[fetch.words.size()-2] ^= 1; // Change wait count without changing attributes.
    auto modified=Gcn::ParseFetchShader(info);
    assert(modified && *original!=*modified);
    std::printf("PASS vertex fetch translation pulling=%u typed=%u index_vgpr=%u\n",
                pulling, typed, instance_vgpr);
}

int main() {
    std::puts("vertex fetch translation: production decoder and SPIR-V");
    for (bool pulling : {false, true})
        for (bool typed : {false, true})
            for (u32 index : {0u, 1u, 2u, 3u}) Compile(pulling, typed, index);
}
