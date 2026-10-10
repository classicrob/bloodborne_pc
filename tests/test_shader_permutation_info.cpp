// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef NDEBUG
#define NDEBUG
#define BB_RESTORE_TEST_ASSERTS
#endif
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/amdgpu/liverpool.h"
#include "gcn_buffer_conformance/gcn_asm.h"
#ifdef BB_RESTORE_TEST_ASSERTS
#undef NDEBUG
#endif
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdlib>

static GcnAsm::Code Fetch(bool second) {
    GcnAsm::Code code;
    code.SLoadDwordx4(4, 2, 0);
    code.SWaitcntAll();
    code.Mubuf(3, {.idxen=true, .vaddr=0, .vdata=4, .srsrc=4});
    if (second) {
        code.SLoadDwordx4(8, 2, 4);
        code.Mubuf(3, {.idxen=true, .vaddr=0, .vdata=8, .srsrc=8});
    } else {
        code.VMov(8, GcnAsm::Inline(0));
    }
    code.SWaitcntAll();
    code.SSetpcB64(0);
    return code;
}

int main() {
    std::fprintf(stderr, "Permutation resource metadata: setup\n");
    setenv("BB_PIPELINE_CACHE", "0", 1);
    Vulkan::Instance instance(0, false);
    Vulkan::Scheduler scheduler(instance);
    AmdGpu::Liverpool liverpool;
    auto& regs = liverpool.regs;
    regs.vs_program.settings.num_user_regs = 16;
    regs.vs_program.settings.num_vgprs = 4;
    regs.vs_program.settings.vgpr_comp_cnt = 3;
    Vulkan::PipelineCache cache(instance, scheduler, &liverpool, 14);
    assert(cache.GetProfile().emulate_vertex_fetch);
    GcnAsm::Code shader;
    shader.SMovLiteral(GcnAsm::VCC_HI, 0x5eed);
    shader.SSwappcB64(0, 0);
    shader.Vop2(37, 4, 8, GcnAsm::Vgpr(4));
    shader.Exp(12, 15, true, 4, 5, 6, 7);
    shader.SEndpgm();
    const auto first_fetch = Fetch(false), second_fetch = Fetch(true);
    std::array<AmdGpu::Buffer, 2> table{};
    for (u32 i = 0; i < table.size(); ++i) {
        auto& sharp = table[i];
        sharp.base_address = 0x20000000ull + i * 256;
        sharp.stride = 16;
        sharp.num_records = 4;
        sharp.data_format = u32(AmdGpu::DataFormat::Format32_32_32_32);
        sharp.num_format = u32(AmdGpu::NumberFormat::Float);
        sharp.dst_sel_x = 4; sharp.dst_sel_y = 5; sharp.dst_sel_z = 6; sharp.dst_sel_w = 7;
    }
    std::array<u32, 16> ud{};
    const uintptr_t table_address = reinterpret_cast<uintptr_t>(table.data());
    std::memcpy(ud.data() + 2, &table_address, sizeof(table_address));
    Vulkan::PipelineSelection selection{};
    selection.regs = &regs;
    auto select = [&](const GcnAsm::Code& fetch) {
        const uintptr_t address = reinterpret_cast<uintptr_t>(fetch.words.data());
        std::memcpy(ud.data(), &address, sizeof(address));
        Shader::Backend::Bindings bindings{};
        return cache.GetProgram(selection, Shader::HwStage::Vertex, Shader::SwStage::Vertex,
            Shader::ShaderParams{ud, shader.words, 0x736f6c337065726dull}, bindings);
    };
    const auto first = select(first_fetch);
    const size_t first_count = std::get<0>(first)->buffers.size();
    const auto second = select(second_fetch);
    const size_t second_count = std::get<0>(second)->buffers.size();
    std::fprintf(stderr, "Permutation resource metadata: first=%zu second=%zu, info pointers %p/%p\n",
        first_count, second_count, std::get<0>(first), std::get<0>(second));
    assert(std::get<1>(first) != std::get<1>(second));
    assert(second_count == first_count + 1);
    const auto again = select(first_fetch);
    assert(std::get<1>(again) == std::get<1>(first));
    assert(std::get<0>(again)->buffers.size() == first_count);
    assert(std::get<0>(first) != std::get<0>(second));
    Vulkan::PrepWorker worker;
    selection.worker = &worker;
    for (bool use_second : {false, true, false, true}) {
        worker.stages.clear();
        const auto result = select(use_second ? second_fetch : first_fetch);
        assert(!worker.failed && worker.stages.size() == 1);
        assert(std::get<0>(result)->buffers.size() == (use_second ? second_count : first_count));
        assert(worker.stages.back().info == std::get<0>(use_second ? second : first));
        assert(std::get<1>(result) == std::get<1>(use_second ? second : first));
    }
    Vulkan::Program preloaded;
    Shader::Info first_metadata = *std::get<0>(first);
    Shader::Info second_metadata = *std::get<0>(second);
    first_metadata.buffers.back().required_binding_bytes = 32;
    second_metadata.buffers.back().required_binding_bytes = 96;
    Shader::StageSpecialization first_spec, second_spec;
    first_spec.info = &first_metadata;
    second_spec.info = &second_metadata;
    preloaded.InsertPermut(std::get<1>(first), std::move(first_spec), std::move(first_metadata), 0);
    const auto* stable_info = preloaded.modules[0].info.get();
    preloaded.InsertPermut(std::get<1>(second), std::move(second_spec), std::move(second_metadata), 9);
    assert(preloaded.modules[0].info.get() == stable_info);
    assert(preloaded.modules[0].info->buffers.size() == first_count);
    assert(preloaded.modules[9].info->buffers.size() == second_count);
    assert(preloaded.modules[0].info_template->buffers.back().required_binding_bytes == 32);
    assert(preloaded.modules[9].info_template->buffers.back().required_binding_bytes == 96);
    preloaded.modules[0].info->buffers.back().required_binding_bytes = 128;
    assert(preloaded.modules[0].info_template->buffers.back().required_binding_bytes == 32);
    auto duplicate_info = *preloaded.modules[0].info_template;
    auto duplicate_spec = preloaded.modules[0].spec;
    preloaded.InsertPermut(std::get<1>(first), std::move(duplicate_spec),
                          std::move(duplicate_info), 0);
    assert(preloaded.modules[0].info.get() == stable_info);
    assert(preloaded.modules[0].info->buffers.back().required_binding_bytes == 128);
    std::puts("Permutation resource metadata: GPU + worker + preloaded storage PASS");
}
