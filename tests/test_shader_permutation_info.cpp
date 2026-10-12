// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef NDEBUG
#define NDEBUG
#define BB_RESTORE_TEST_ASSERTS
#endif
#ifdef __APPLE__
#include <fstream>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
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
#include <csignal>
#include <csetjmp>

extern "C" __thread sigjmp_buf* runtime_fault_recover;

static volatile sig_atomic_t recovered_reads = 0;

static void RecoverWorkerRead(int signal, siginfo_t*, void*) {
    if (runtime_fault_recover) {
        recovered_reads = recovered_reads + 1;
        sigset_t unblock;
        sigemptyset(&unblock);
        sigaddset(&unblock, signal);
        sigprocmask(SIG_UNBLOCK, &unblock, nullptr);
        siglongjmp(*runtime_fault_recover, 1);
    }
    std::_Exit(3);
}

// Two cached SRT layouts, both producing the descriptor at flat[4..7], but reading
// different root pointers. Install them in actual compiled permutations below to isolate
// candidate walking from the compiler's choice of SRT layout.
template <u32 Root>
static void CandidateTable(const u32* data, u32* flat) {
    uintptr_t address;
    std::memcpy(&address, data + Root, sizeof(address));
    const auto* source = reinterpret_cast<const volatile u32*>(address);
    flat[4] = 0xbadbad;
    for (u32 i = 0; i < 4; ++i) flat[4 + i] = source[i];
}

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
    {
        // P0 2026-10-12: a selection forgets the previous draw's stages, runtime infos included
        // (an FS-less draw after a depth-bounds FS draw built an FS-less pipeline with a
        // depth-bounds layout).
        const u32 fs = u32(Shader::SwStage::Fragment);
        Vulkan::PipelineSelection stale{};
        stale.runtime_infos[fs].hw.fs.depth_bounds_test = true;
        stale.runtime_infos[fs].hw.fs.clip_distance_emulation = true;
        stale.modules[fs] = vk::ShaderModule{reinterpret_cast<VkShaderModule>(uintptr_t(0x10))};
        stale.ResetStages();
        std::array<vk::ShaderModule, Vulkan::MaxShaderStages> modules{};
        assert(!stale.runtime_infos[fs].hw.fs.depth_bounds_test && !stale.modules[fs] &&
               !stale.infos[fs] && Vulkan::FragmentInfoMatchesShader(stale.runtime_infos, modules));
        // The pipeline-side check: a fragment runtime info needs a fragment shader.
        std::array<Shader::RuntimeInfo, Vulkan::MaxShaderStages> infos{};
        for (auto& info : infos) {
            std::memset(&info, 0, sizeof(info));
        }
        assert(Vulkan::FragmentInfoMatchesShader(infos, modules));
        infos[fs].hw.fs.depth_bounds_test = true;
        assert(!Vulkan::FragmentInfoMatchesShader(infos, modules)); // stale, no FS: refused
        modules[fs] = vk::ShaderModule{reinterpret_cast<VkShaderModule>(uintptr_t(0x10))};
        assert(Vulkan::FragmentInfoMatchesShader(infos, modules)); // a real depth-bounds FS
        std::fprintf(stderr, "Stale fragment runtime info: reset by the selection, refused "
                             "without a fragment shader: PASS\n");
    }
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
    regs.vgt_gs_out_prim_type.outprim_type = AmdGpu::GsOutputPrimitiveType::TriangleStrip;
    // The value read through the user-data V# (s[4:7]) is exported, not stored: a GS copy shader
    // only reads the GSVS ring, exports and stream-outs, and the copy-VS role stops on any other
    // memory write (ring_access_elimination.cpp, CopyShaderAccesses). Position 0 is also ESGS
    // record dwords 0-3 for the ES role (ring item size 4).
    GcnAsm::Code shared_shader;
    shared_shader.SBufferLoad(1, 12, 4, true, 0);
    shared_shader.SWaitcntAll();
    shared_shader.VMov(4, 12);
    shared_shader.Exp(12, 0xF, true, 4, 4, 4, 4);
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
    // Same-role permutations also retain their last selected draw's data when another
    // permutation is probed and selected. Distinct strides force distinct specializations.
    selection.worker = nullptr;
    regs.stage_enable.raw = AmdGpu::ShaderStageEnable::VgtStages::Vs;
    std::array<u32, 16> same_a_ud = es_ud, same_b_ud = copy_ud;
    auto same_b_table = copy_table;
    same_b_table[0].stride = 32;
    std::memcpy(same_b_ud.data() + 4, same_b_table.data(), sizeof(same_b_table));
    const auto same_select = [&](const std::array<u32, 16>& data) {
        Shader::Backend::Bindings bindings{};
        return cache.GetProgram(selection, Shader::HwStage::Vertex, Shader::SwStage::Vertex,
            Shader::ShaderParams{data, shared_shader.words, 0x73616d65726f6c65ull}, bindings);
    };
    const auto same_a = same_select(same_a_ud);
    const auto* same_a_info = std::get<0>(same_a);
    const auto same_a_flat = same_a_info->flattened_ud_buf;
    const auto same_b = same_select(same_b_ud);
    const auto* same_b_info = std::get<0>(same_b);
    const auto same_b_flat = same_b_info->flattened_ud_buf;
    assert(same_a_info != same_b_info);
    assert(std::get<1>(same_a) != std::get<1>(same_b));
    assert(same_a_info->user_data.data() == same_a_ud.data());
    assert(same_a_info->flattened_ud_buf == same_a_flat);
    for (unsigned repeat = 0; repeat < 8; ++repeat) {
        assert(std::get<0>(same_select(same_a_ud)) == same_a_info);
        assert(same_b_info->user_data.data() == same_b_ud.data());
        assert(same_b_info->flattened_ud_buf == same_b_flat);
        assert(std::get<0>(same_select(same_b_ud)) == same_b_info);
        assert(same_a_info->user_data.data() == same_a_ud.data());
        assert(same_a_info->flattened_ud_buf == same_a_flat);
    }
    Vulkan::PrepWorker same_worker;
    selection.worker = &same_worker;
    const auto prepared_a = same_select(same_a_ud);
    const auto prepared_a_flat = std::get<0>(prepared_a)->flattened_ud_buf;
    const auto prepared_b = same_select(same_b_ud);
    const auto prepared_b_flat = std::get<0>(prepared_b)->flattened_ud_buf;
    assert(!same_worker.failed && same_worker.stages.size() == 2);
    assert(std::get<1>(prepared_a) == std::get<1>(same_a));
    assert(std::get<1>(prepared_b) == std::get<1>(same_b));
    for (unsigned repeat = 0; repeat < 8; ++repeat) {
        same_worker.stages.clear();
        assert(std::get<0>(same_select(same_a_ud)) == std::get<0>(prepared_a));
        assert(std::get<0>(prepared_b)->user_data.data() == same_b_ud.data());
        assert(std::get<0>(prepared_b)->flattened_ud_buf == prepared_b_flat);
        assert(std::get<0>(same_select(same_b_ud)) == std::get<0>(prepared_b));
        assert(std::get<0>(prepared_a)->user_data.data() == same_a_ud.data());
        assert(std::get<0>(prepared_a)->flattened_ud_buf == prepared_a_flat);
    }
    assert(same_a_info->flattened_ud_buf == same_a_flat);
    assert(same_b_info->flattened_ud_buf == same_b_flat);
    selection.worker = nullptr;
    Shader::Info::ud_snapshots[0] = {same_a_info, same_b_ud.data(), u32(same_b_ud.size()),
        same_b_flat.data(), u32(same_b_flat.size()), 0x1234};
    Shader::Info::num_ud_snapshots = 1;
    assert(std::get<0>(same_select(same_a_ud)) == same_a_info);
    assert(Shader::Info::num_ud_snapshots == 1);
    assert(same_a_info->ProgramBase() == 0x1234);
    assert(same_a_info->UserData().data() == same_b_ud.data());
    assert(same_a_info->FlatUserData().data() == same_b_flat.data());

    // A speculative worker's table read can fault. Recovery must pop only its temporary
    // view, retain the caller's view, and leave previously selected permutations untouched.
    struct sigaction recover_action{}, old_segv{}, old_bus{};
    recover_action.sa_sigaction = RecoverWorkerRead;
    recover_action.sa_flags = SA_SIGINFO;
    sigemptyset(&recover_action.sa_mask);
    assert(sigaction(SIGSEGV, &recover_action, &old_segv) == 0);
    assert(sigaction(SIGBUS, &recover_action, &old_bus) == 0);
    const uintptr_t invalid_table = 1;
    std::memcpy(ud.data() + 2, &invalid_table, sizeof(invalid_table));
    Vulkan::PrepWorker fault_worker;
    selection.worker = &fault_worker;
    select(first_fetch);
    assert(fault_worker.failed && fault_worker.stages.empty());
    assert(runtime_fault_recover == nullptr);
    assert(Shader::Info::num_ud_snapshots == 1);
    assert(same_a_info->ProgramBase() == 0x1234);
    assert(same_a_info->FlatUserData().data() == same_b_flat.data());
    std::memcpy(ud.data() + 2, &table_address, sizeof(table_address));
    fault_worker.failed = false;
    assert(std::get<1>(select(first_fetch)) == std::get<1>(first));
    assert(!fault_worker.failed && fault_worker.stages.size() == 1);
    assert(Shader::Info::num_ud_snapshots == 1);

#ifdef __APPLE__
    // A worker meeting a descriptor with no number conversion abandons the preparation: no stage
    // is published, and the flag, snapshot count and recovery point are restored. With the
    // descriptor valid again the worker hits. The GPU thread's own selection of the bad
    // descriptor still stops (a child process).
    {
        const auto saved = table[0];
        table[0].num_format = 6; // with Format32_32_32_32: no conversion here (the hunts' stop)
        Vulkan::PrepWorker reject_worker;
        selection.worker = &reject_worker;
        const auto rejections = Shader::speculative_rejections.load();
        const u32 snapshots = Shader::Info::num_ud_snapshots;
        select(first_fetch);
        assert(reject_worker.failed && reject_worker.stages.empty());
        assert(Shader::speculative_rejections.load() == rejections + 1);
        assert(!Shader::speculative_selection);
        assert(runtime_fault_recover == nullptr);
        assert(Shader::Info::num_ud_snapshots == snapshots);
        table[0] = saved;
        reject_worker.failed = false;
        assert(std::get<1>(select(first_fetch)) == std::get<1>(first));
        assert(!reject_worker.failed && reject_worker.stages.size() == 1);
        selection.worker = nullptr;
        table[0].num_format = 6;
        // The child's output goes to a file: the stop must be the conversion stop (exit 23 with
        // its message), not some other failure of a process forked after Vulkan started threads.
        char log_path[] = "/tmp/permutation-stop-XXXXXX";
        const int log_fd = mkstemp(log_path);
        assert(log_fd >= 0);
        std::fflush(nullptr);
        const pid_t child = fork();
        assert(child >= 0);
        if (child == 0) {
            dup2(log_fd, STDOUT_FILENO);
            dup2(log_fd, STDERR_FILENO);
            select(first_fetch); // the GPU thread's selection: stops
            std::_Exit(0);
        }
        close(log_fd);
        int status = 0;
        const pid_t waited = waitpid(child, &status, 0);
        std::stringstream child_log;
        child_log << std::ifstream(log_path).rdbuf();
        unlink(log_path);
        const std::string text = child_log.str();
        if (waited != child || !WIFEXITED(status) || WEXITSTATUS(status) != 23 ||
            text.find("which this backend cannot convert") == std::string::npos) {
            std::fprintf(stderr, "GPU-thread control: wait %d, status %#x, log:\n%s\n", int(waited),
                         status, text.c_str());
            std::abort();
        }
        table[0] = saved;
        std::puts("Worker selection abandons an unconvertible descriptor; GPU thread stops PASS");
    }
#endif

    // On the direct path, try the last-used (wrong-layout) permutation first. Its missing
    // table must reject only that candidate, despite a partial write to the scratch flat
    // buffer. The following candidate must hit, without recompiling or publishing bad data.
    auto* layout_a_info = const_cast<Shader::Info*>(same_a_info);
    auto* layout_b_info = const_cast<Shader::Info*>(same_b_info);
    const auto old_a_walker = layout_a_info->srt_info.walker_func;
    const auto old_b_walker = layout_b_info->srt_info.walker_func;
    layout_a_info->srt_info.walker_func = CandidateTable<0>;
    layout_b_info->srt_info.walker_func = CandidateTable<2>;
    const auto a_descriptor = table[0];
    const auto b_descriptor = same_b_table[0];
    const uintptr_t a_table = reinterpret_cast<uintptr_t>(&a_descriptor);
    const uintptr_t b_table = reinterpret_cast<uintptr_t>(&b_descriptor);
    std::memcpy(same_a_ud.data(), &a_table, sizeof(a_table));
    std::memcpy(same_b_ud.data() + 2, &b_table, sizeof(b_table));
    selection.worker = nullptr;
    const sig_atomic_t before_reads = recovered_reads;
    sigjmp_buf outer_recovery;
    if (sigsetjmp(outer_recovery, 1)) {
        std::fputs("Candidate SRT fault escaped to the caller's recovery point\n", stderr);
        return 2;
    }
    runtime_fault_recover = &outer_recovery;
    for (unsigned repeat = 0; repeat < 8; ++repeat) {
        const auto b_before = same_b_info->flattened_ud_buf;
        const auto hit_a = same_select(same_a_ud);
        assert(std::get<0>(hit_a) == same_a_info);
        assert(std::get<1>(hit_a) == std::get<1>(same_a));
        assert(std::get<3>(hit_a) == std::get<3>(same_a));
        assert(same_b_info->flattened_ud_buf == b_before);
        assert(same_b_info->user_data.data() == same_b_ud.data());
        const auto a_before = same_a_info->flattened_ud_buf;
        const auto hit_b = same_select(same_b_ud);
        assert(std::get<0>(hit_b) == same_b_info);
        assert(std::get<1>(hit_b) == std::get<1>(same_b));
        assert(std::get<3>(hit_b) == std::get<3>(same_b));
        assert(same_a_info->flattened_ud_buf == a_before);
        assert(same_a_info->user_data.data() == same_a_ud.data());
        assert(runtime_fault_recover == &outer_recovery);
        assert(Shader::Info::num_ud_snapshots == 1);
        assert(same_a_info->ProgramBase() == 0x1234);
        assert(same_a_info->FlatUserData().data() == same_b_flat.data());
    }
    assert(recovered_reads - before_reads >= 15);
    runtime_fault_recover = nullptr;
    layout_a_info->srt_info.walker_func = old_a_walker;
    layout_b_info->srt_info.walker_func = old_b_walker;
    std::puts("Unreadable SRT candidates: two layouts, 16 cache hits, outer recovery preserved PASS");
    assert(sigaction(SIGSEGV, &old_segv, nullptr) == 0);
    assert(sigaction(SIGBUS, &old_bus, nullptr) == 0);
    Shader::Info::num_ud_snapshots = 0;
    selection.worker = nullptr;
    std::puts("Same-role rejected permutations retain user data: direct + worker PASS");
    std::puts("Temporary specialization views preserve outer snapshots and recover faults PASS");
    // Tessellation specializations fill fields from the constant buffer after the raw
    // register state is built. Repeated lookups must compare the constructed specs.
    selection.worker = nullptr;
    regs.stage_enable.raw = AmdGpu::ShaderStageEnable::VgtStages::LsHs;
    regs.hs_program.settings = regs.vs_program.settings;
    regs.ls_hs_config.hs_input_control_points = 3;
    regs.ls_hs_config.hs_output_control_points = 3;
    regs.tess_config.type = AmdGpu::TessellationType::Triangle;
    regs.tess_config.topology = AmdGpu::TessellationTopology::TriangleCw;
    regs.tess_config.partitioning = AmdGpu::TessellationPartitioning::Integer;
    Shader::TessellationDataConstantBuffer tess_constants{};
    tess_constants.ls_stride = 16;
    tess_constants.hs_cp_stride = 16;
    tess_constants.num_patches = 1;
    tess_constants.hs_output_base = 48;
    tess_constants.patch_const_base = 96;
    tess_constants.patch_output_size = 48;
    AmdGpu::Buffer tess_buffer{};
    const auto tess_address = reinterpret_cast<uintptr_t>(&tess_constants);
    assert(tess_address < (1ull << 44));
    tess_buffer.base_address = tess_address;
    tess_buffer.num_records = sizeof(tess_constants);
    std::array<u32, 16> tess_ud{};
    std::memcpy(tess_ud.data() + 4, &tess_buffer, sizeof(tess_buffer));
    for (bool hull : {true, false}) {
        GcnAsm::Code tess_shader;
        tess_shader.SBufferLoad(1, 12, 4, true, 3);
        tess_shader.SWaitcntAll();
        tess_shader.VMov(4, 12);
        if (hull) {
            tess_shader.VMovLiteral(5, 0x3f800000);
            tess_shader.Ds(13, 4, 5, 0);
        } else {
            tess_shader.Ds(54, 4, 0, 5);
            tess_shader.SWaitcntAll();
            tess_shader.Exp(12, 15, true, 5, 5, 5, 5);
        }
        tess_shader.SEndpgm();
        const auto select_tess = [&]() {
            Shader::Backend::Bindings bindings{};
            return cache.GetProgram(selection,
                hull ? Shader::HwStage::Hull : Shader::HwStage::Vertex,
                hull ? Shader::SwStage::TessellationControl : Shader::SwStage::TessellationEval,
                Shader::ShaderParams{tess_ud, tess_shader.words, 0x7465737372657573ull + hull},
                bindings);
        };
        const auto compiled = select_tess();
        for (unsigned repeat = 0; repeat < 16; ++repeat) {
            const auto hit = select_tess();
            assert(std::get<0>(hit) == std::get<0>(compiled));
            assert(std::get<1>(hit) == std::get<1>(compiled));
            assert(std::get<3>(hit) == std::get<3>(compiled));
        }
        selection.worker = &worker;
        worker.stages.clear();
        const auto prepared = select_tess();
        assert(!worker.failed && worker.stages.size() == 1);
        assert(std::get<1>(prepared) == std::get<1>(compiled));
        selection.worker = nullptr;
        std::printf("%s derived-constant specialization: 16 cache hits + worker hit PASS\n",
                    hull ? "TCS" : "TES");
    }
    std::puts("Permutation resource metadata: GPU + worker + preloaded storage + image binding "
              "counts PASS");
}
