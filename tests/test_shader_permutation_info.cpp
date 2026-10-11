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
#include <memory>
#include <utility>

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
    // Image descriptor counts (DynamicIndex mip fallback: one binding per mip level). The
    // specialization records what the emitter declares, also for an image whose T# has no
    // address but a mip range; the stored permutation records it, and a cache hit or a
    // preloaded stage advances the bindings by those counts, not by one per image.
    AmdGpu::Image null_tsharp{};
    null_tsharp.base_address = 0;
    null_tsharp.type = u64(AmdGpu::ImageType::Color2D);
    null_tsharp.data_format = u64(AmdGpu::DataFormat::Format8_8_8_8);
    null_tsharp.num_format = u64(AmdGpu::NumberFormat::Unorm);
    null_tsharp.base_level = 0;
    null_tsharp.last_level = 3;
    Shader::ImageResource mip_image{};
    mip_image.is_written = true;
    mip_image.mip_fallback_mode = Shader::MipStorageFallbackMode::DynamicIndex;
    std::memcpy(mip_image.sharp_fetch.immediates.data(), &null_tsharp, sizeof(null_tsharp));
    mip_image.sharp_fetch.offsets.fill(0);
    mip_image.sharp_fetch.load_mask = 0;
    Shader::ImageResource plain_image = mip_image;
    plain_image.mip_fallback_mode = Shader::MipStorageFallbackMode::None;
    Shader::Info image_info{};
    image_info.pgm_hash = 0x1111;
    image_info.images.push_back(mip_image);
    image_info.images.push_back(plain_image);
    image_info.flattened_ud_buf.resize(16);
    const Shader::StageSpecialization image_spec(image_info, Shader::RuntimeInfo{},
                                                 cache.GetProfile(), {});
    std::fprintf(stderr, "Image binding counts: specialized %u, %u\n",
                 image_spec.images[0].num_bindings, image_spec.images[1].num_bindings);
    assert(image_spec.images[0].num_bindings == 4);
    assert(image_spec.images[1].num_bindings == 1);
    // Specializations differing only in a null image's mip count select different
    // permutations, in both directions: with every resource null, and with a valid one too.
    const auto spec_with_levels = [&](u32 last_level, bool valid_plain) {
        AmdGpu::Image tsharp = null_tsharp;
        tsharp.last_level = last_level;
        Shader::Info variant = image_info;
        std::memcpy(variant.images[0].sharp_fetch.immediates.data(), &tsharp, sizeof(tsharp));
        if (valid_plain) {
            AmdGpu::Image valid = null_tsharp;
            valid.base_address = 0x40000000ull >> 8;
            std::memcpy(variant.images[1].sharp_fetch.immediates.data(), &valid, sizeof(valid));
        }
        auto info_copy = std::make_shared<Shader::Info>(variant);
        return std::pair{info_copy, Shader::StageSpecialization(*info_copy, Shader::RuntimeInfo{},
                                                                cache.GetProfile(), {})};
    };
    for (bool valid_plain : {false, true}) {
        const auto four = spec_with_levels(3, valid_plain);
        const auto two = spec_with_levels(1, valid_plain);
        const auto four_again = spec_with_levels(3, valid_plain);
        std::fprintf(stderr, "Image binding counts: null mips 4 vs 2 (valid other image %d): %u/%u\n",
                     valid_plain, four.second.images[0].num_bindings,
                     two.second.images[0].num_bindings);
        assert(four.second.images[0].num_bindings == 4 && two.second.images[0].num_bindings == 2);
        assert(!(four.second == two.second) && !(two.second == four.second));
        assert(four.second == four_again.second);
    }
    Vulkan::Program image_program;
    Shader::StageSpecialization stored_spec = image_spec;
    image_program.InsertPermut(std::get<1>(first), std::move(stored_spec), Shader::Info{image_info},
                               0);
    const auto& stored = *image_program.modules[0].info;
    assert(stored.images[0].permutation_bindings == 4);
    assert(stored.images[1].permutation_bindings == 1);
    assert(image_program.modules[0].info_template->images[0].permutation_bindings == 4);
    // A later stage starts after all 5 image descriptors (compile and cache hit alike).
    Shader::Backend::Bindings after{};
    stored.AddBindings(after);
    std::fprintf(stderr, "Image binding counts: stored stage advances unified by %u\n",
                 after.unified);
    assert(after.unified == stored.buffers.size() + 5 + stored.samplers.size());
    // A program hash can be shared by distinct hardware stages (the game's GS and copy VS).
    // Looking up the second stage must not refresh a rejected first-stage permutation with
    // the second stage's registers. Exercise compute and copy roles through GetProgram itself.
    selection.worker = nullptr;
    regs.stage_enable.raw = AmdGpu::ShaderStageEnable::VgtStages::EsGs;
    regs.es_program.settings = regs.vs_program.settings;
    regs.vgt_esgs_ring_itemsize = 4;
    regs.vgt_gs_max_vert_out = 3;
    regs.vgt_gs_vert_itemsize[0] = 4;
    GcnAsm::Code shared_shader;
    shared_shader.SBufferLoad(1, 12, 4, true, 0);
    shared_shader.SWaitcntAll();
    shared_shader.VMov(4, 12);
    shared_shader.Mubuf(28, {.vdata=4, .srsrc=8});
    shared_shader.SEndpgm();
    std::array<u32, 16> es_ud{}, copy_ud{};
    std::memcpy(es_ud.data() + 4, table.data(), sizeof(table));
    auto copy_table = table;
    copy_table[0].base_address = 0x34560000;
    copy_table[0].num_records = 0x1234;
    std::memcpy(copy_ud.data() + 4, copy_table.data(), sizeof(copy_table));
    const auto shared_select = [&](Shader::HwStage stage, const std::array<u32, 16>& data) {
        Shader::Backend::Bindings bindings{};
        return cache.GetProgram(selection, stage, Shader::SwStage::Vertex,
            Shader::ShaderParams{data, shared_shader.words, 0x73616d6568617368ull}, bindings);
    };
    const auto es_result = shared_select(Shader::HwStage::Export, es_ud);
    const auto* es_info = std::get<0>(es_result);
    const auto es_flat = es_info->flattened_ud_buf;
    const auto copy_result = shared_select(Shader::HwStage::Vertex, copy_ud);
    const auto* copy_info = std::get<0>(copy_result);
    const auto copy_flat = copy_info->flattened_ud_buf;
    assert(es_info != copy_info);
    assert(es_info->user_data.data() == es_ud.data());
    assert(es_info->flattened_ud_buf == es_flat);
    assert(copy_info->user_data.data() == copy_ud.data());
    for (unsigned repeat = 0; repeat < 3; ++repeat) {
        assert(std::get<0>(shared_select(Shader::HwStage::Export, es_ud)) == es_info);
        assert(copy_info->user_data.data() == copy_ud.data());
        assert(copy_info->flattened_ud_buf == copy_flat);
        assert(std::get<0>(shared_select(Shader::HwStage::Vertex, copy_ud)) == copy_info);
        assert(es_info->user_data.data() == es_ud.data());
        assert(es_info->flattened_ud_buf == es_flat);
    }
    std::puts("Shared-hash compute/copy roles retain their own user-data banks PASS");
    std::puts("Permutation resource metadata: GPU + worker + preloaded storage + image binding "
              "counts PASS");
}
