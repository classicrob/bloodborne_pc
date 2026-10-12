// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_set>
#include "shader_recompiler/info.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/fill_clear.h"
#include "video_core/renderer_vulkan/vk_shader_hle.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/host_shaders/buffer_multi_copy_comp.h"
#include "bbport_toggles.h"
#include "video_core/renderer_vulkan/vk_gpu_profiler.h"

extern std::unique_ptr<AmdGpu::Liverpool> liverpool;

namespace Vulkan {

static constexpr u64 COPY_SHADER_HASH = 0xfefebf9f;

// bbport: the copy shader runs ~57 times per frame with ~1024 small ranges each. As one
// vkCmdCopyBuffer with that many regions it cost ~37 ns of GPU time per range (~2.1 ms per
// frame) plus the recording; buffer_multi_copy.comp copies all ranges in one dispatch.
static bool MultiCopy(Rasterizer& rasterizer, const VideoCore::Buffer* src,
                      const VideoCore::Buffer* dst, std::span<const vk::BufferCopy> copies) {
    if (copies.size() < 8 || BbToggle::Disabled(BbToggle::MultiCopyShader)) {
        return false;
    }
    u64 src_min = ~0ull, src_max = 0, dst_min = ~0ull, dst_max = 0;
    for (const auto& copy : copies) {
        if ((copy.srcOffset | copy.dstOffset | copy.size) & 3) {
            return false; // dword copies only
        }
        src_min = std::min(src_min, copy.srcOffset);
        src_max = std::max(src_max, copy.srcOffset + copy.size);
        dst_min = std::min(dst_min, copy.dstOffset);
        dst_max = std::max(dst_max, copy.dstOffset + copy.size);
    }
    auto& runtime = rasterizer.GetRuntime();
    auto& scheduler = runtime.GetScheduler();
    const auto& instance = runtime.GetInstance();
    const u64 align = instance.StorageMinAlignment();
    src_min = Common::AlignDown(src_min, align);
    dst_min = Common::AlignDown(dst_min, align);

    struct Pipeline {
        vk::UniqueDescriptorSetLayout set_layout;
        vk::UniquePipelineLayout layout;
        vk::UniquePipeline pipeline;
    };
    static Pipeline pipe = [&] {
        const auto device = instance.GetDevice();
        Pipeline p;
        std::array<vk::DescriptorSetLayoutBinding, 3> bindings{};
        for (u32 i = 0; i < 3; ++i) {
            bindings[i] = {.binding = i,
                           .descriptorType = vk::DescriptorType::eStorageBuffer,
                           .descriptorCount = 1,
                           .stageFlags = vk::ShaderStageFlagBits::eCompute};
        }
        p.set_layout = Check(device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = static_cast<u32>(bindings.size()),
            .pBindings = bindings.data(),
        }));
        const vk::PushConstantRange range{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                                          .offset = 0,
                                          .size = sizeof(u32)};
        p.layout = Check(device.createPipelineLayoutUnique({
            .setLayoutCount = 1,
            .pSetLayouts = &*p.set_layout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &range,
        }));
        const auto module = CompileSPV(BUFFER_MULTI_COPY_COMP, device);
        p.pipeline = Check(device.createComputePipelineUnique(
            {}, vk::ComputePipelineCreateInfo{
                    .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                              .module = module,
                              .pName = "main"},
                    .layout = *p.layout,
                }));
        device.destroyShaderModule(module);
        return p;
    }();

    // The region table goes into the stream buffer (host writes, visible at submission).
    thread_local std::vector<u32> regions;
    regions.resize(copies.size() * 4);
    for (size_t i = 0; i < copies.size(); ++i) {
        regions[i * 4 + 0] = static_cast<u32>((copies[i].srcOffset - src_min) / 4);
        regions[i * 4 + 1] = static_cast<u32>((copies[i].dstOffset - dst_min) / 4);
        regions[i * 4 + 2] = static_cast<u32>(copies[i].size / 4);
        regions[i * 4 + 3] = 0;
    }
    auto& stream = rasterizer.GetBufferCache().GetStreamBuffer();
    const u64 table_size = regions.size() * sizeof(u32);
    const u64 table_offset = stream.Copy(regions.data(), table_size, align);

    if (auto* profiler = GpuProfiler::Get()) {
        profiler->Mark(0xC0B1ull, [] { return std::string{"copy shader HLE: barrier + dispatch"}; });
    }
    scheduler.EndRendering();
    const u64 src_size = src_max - src_min, dst_size = dst_max - dst_min;
    if (runtime.IsBufferAccessed(src, src_min, src_size) ||
        runtime.IsBufferAccessed(dst, dst_min, dst_size, true)) {
        runtime.FlushBarriers();
    }
    const u32 count = static_cast<u32>(copies.size());
    scheduler.Record([src = src->Handle(), dst = dst->Handle(), table = stream.Handle(), src_min,
                      src_size, dst_min, dst_size, table_offset, table_size,
                      count](vk::CommandBuffer cmdbuf) {
        const std::array<vk::DescriptorBufferInfo, 3> infos{{
            {src, src_min, src_size},
            {dst, dst_min, dst_size},
            {table, table_offset, table_size},
        }};
        std::array<vk::WriteDescriptorSet, 3> writes{};
        for (u32 i = 0; i < 3; ++i) {
            writes[i] = {.dstBinding = i,
                         .descriptorCount = 1,
                         .descriptorType = vk::DescriptorType::eStorageBuffer,
                         .pBufferInfo = &infos[i]};
        }
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *pipe.pipeline);
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *pipe.layout, 0, writes);
        cmdbuf.pushConstants(*pipe.layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(count),
                             &count);
        cmdbuf.dispatch(count, 1, 1);
    });
    runtime.AccessBuffer(src, src_min, src_size, vk::PipelineStageFlagBits2::eComputeShader,
                         vk::AccessFlagBits2::eShaderRead);
    runtime.AccessBuffer(dst, dst_min, dst_size, vk::PipelineStageFlagBits2::eComputeShader,
                         vk::AccessFlagBits2::eShaderWrite);
    return true;
}

static bool ExecuteCopyShaderHLE(const Shader::Info& info, const AmdGpu::ComputeProgram& cs_program,
                                 Rasterizer& rasterizer) {
    auto& runtime = rasterizer.GetRuntime();
    auto& buffer_cache = rasterizer.GetBufferCache();

    // Copy shader defines three formatted buffers as inputs: control, source, and destination.
    const auto ctl_buf_sharp = info.buffers[0].GetSharp(info);
    const auto src_buf_sharp = info.buffers[1].GetSharp(info);
    const auto dst_buf_sharp = info.buffers[2].GetSharp(info);
    const auto buf_stride = src_buf_sharp.GetStride();
    ASSERT(buf_stride == dst_buf_sharp.GetStride());

    struct CopyShaderControl {
        u32 dst_idx;
        u32 src_idx;
        u32 end;
    };
    static_assert(sizeof(CopyShaderControl) == 12);
    ASSERT(ctl_buf_sharp.GetStride() == sizeof(CopyShaderControl));
    const auto ctl_buf = reinterpret_cast<const CopyShaderControl*>(ctl_buf_sharp.base_address);

    static std::vector<vk::BufferCopy> copies;
    copies.clear();
    copies.reserve(cs_program.dim_x);

    for (u32 i = 0; i < cs_program.dim_x; i++) {
        const auto& [dst_idx, src_idx, end] = ctl_buf[i];
        const u32 local_dst_offset = dst_idx * buf_stride;
        const u32 local_src_offset = src_idx * buf_stride;
        const u32 local_size = (end + 1) * buf_stride;
        copies.emplace_back(local_src_offset, local_dst_offset, local_size);
    }

    // bbport: 64 KiB instead of 64 MiB. The copies are a few KiB spread over up to 57 MiB, and
    // each batch synchronizes (uploads, marks GPU-modified) its whole range: GPU time of the copy
    // shader 1.5 -> 0.6 ms/frame, GPU busy 85% -> 77%, frame rate no lower.
    // BB_COPY_MERGE_KB overrides it.
    static const vk::DeviceSize MaxDistanceForMerge = [] {
        const char* env = std::getenv("BB_COPY_MERGE_KB");
        return env ? vk::DeviceSize(std::strtoull(env, nullptr, 10)) * 1024 : vk::DeviceSize(64_KB);
    }();
    u32 batch_start = 0;
    u32 batch_end = 0;

    while (batch_end < copies.size()) {
        // Place first copy into the current batch
        const auto& copy = copies[batch_start];
        auto src_offset_min = copy.srcOffset;
        auto src_offset_max = copy.srcOffset + copy.size;
        auto dst_offset_min = copy.dstOffset;
        auto dst_offset_max = copy.dstOffset + copy.size;

        for (++batch_end; batch_end < copies.size(); batch_end++) {
            // Compute new src and dst bounds if we were to batch this copy
            const auto& [src_offset, dst_offset, size] = copies[batch_end];
            auto new_src_offset_min = std::min(src_offset_min, src_offset);
            auto new_src_offset_max = std::max(src_offset_max, src_offset + size);
            if (new_src_offset_max - new_src_offset_min > MaxDistanceForMerge) {
                break;
            }

            auto new_dst_offset_min = std::min(dst_offset_min, dst_offset);
            auto new_dst_offset_max = std::max(dst_offset_max, dst_offset + size);
            if (new_dst_offset_max - new_dst_offset_min > MaxDistanceForMerge) {
                break;
            }

            // We can batch this copy
            src_offset_min = new_src_offset_min;
            src_offset_max = new_src_offset_max;
            dst_offset_min = new_dst_offset_min;
            dst_offset_max = new_dst_offset_max;
        }

        // Obtain buffers for the total source and destination ranges.
        const auto [src_buf, src_buf_offset] = buffer_cache.ObtainBuffer(
            src_buf_sharp.base_address + src_offset_min, src_offset_max - src_offset_min, false);
        const auto [dst_buf, dst_buf_offset] = buffer_cache.ObtainBuffer(
            dst_buf_sharp.base_address + dst_offset_min, dst_offset_max - dst_offset_min, true);

        // Apply found buffer base.
        const auto vk_copies = std::span{copies}.subspan(batch_start, batch_end - batch_start);
        for (auto& copy : vk_copies) {
            copy.srcOffset = copy.srcOffset - src_offset_min + src_buf_offset;
            copy.dstOffset = copy.dstOffset - dst_offset_min + dst_buf_offset;
        }

        // Execute buffer copies.
        LOG_TRACE(Render_Vulkan, "HLE buffer copy: src_size = {}, dst_size = {}",
                  src_offset_max - src_offset_min, dst_offset_max - dst_offset_min);
        if (!MultiCopy(rasterizer, src_buf, dst_buf, vk_copies)) {
            runtime.CopyBuffer(src_buf, dst_buf, vk_copies);
        }
        batch_start = batch_end;
    }

    return true;
}

#ifdef __APPLE__
// bbport (BB_RT_KERNEL_CENSUS=1, diagnostic): the SDK fill (0x8b355b5a) and copy (0x3d5ebf4e)
// compute kernels write render-target memory through storage buffers, which costs a full
// buffer<->image round trip per frame for every image they touch. Logs each distinct call shape
// (the kernel's buffer V#s, the dispatch size and the images overlapping each buffer) once, as
// the input for exact image-level replacements. Changes nothing.
static constexpr u64 SDK_FILL_HASH = 0x8b355b5a;
static constexpr u64 SDK_COPY_HASH = 0x3d5ebf4e;

static void CensusRenderTargetKernel(const Shader::Info& info,
                                     const AmdGpu::ComputeProgram& cs_program,
                                     Rasterizer& rasterizer) {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_RT_KERNEL_CENSUS");
        return env && env[0] == '1';
    }();
    if (!enabled) {
        return;
    }
    static std::mutex mutex;
    static std::unordered_set<u64> seen;
    std::string text = fmt::format("{} kernel {:#x}: dispatch {}x{}x{}",
                                   info.pgm_hash == SDK_FILL_HASH ? "FILL" : "COPY",
                                   info.pgm_hash, cs_program.dim_x, cs_program.dim_y,
                                   cs_program.dim_z);
    u64 signature = info.pgm_hash ^ (u64(cs_program.dim_x) << 32);
    const auto ud = info.UserData();
    if (ud.size() > 10) {
        text += fmt::format(", s10={}", ud[10]);
    }
    auto& texture_cache = rasterizer.GetTextureCache();
    for (u32 i = 0; i < info.buffers.size(); ++i) {
        const auto& desc = info.buffers[i];
        if (desc.IsSpecial()) {
            continue;
        }
        const auto sharp = desc.GetSharp(info);
        signature = signature * 0x100000001b3ull ^ sharp.base_address ^ (u64(sharp.GetSize()) << 40) ^
                    (u64(sharp.GetDataFmt()) << 56) ^ desc.is_written;
        text += fmt::format("; buffer {} {} base {:#x} size {} stride {} fmt {}/{}", i,
                            desc.is_written ? "W" : "R", sharp.base_address, sharp.GetSize(),
                            sharp.GetStride(), u32(sharp.GetDataFmt()), u32(sharp.GetNumberFmt()));
        if (sharp.base_address && sharp.GetSize()) {
            texture_cache.ForEachImageInRegion(
                sharp.base_address, sharp.GetSize(), [&](VideoCore::ImageId, VideoCore::Image& image) {
                    // Images that appear later (a copy's destination) make a new call shape.
                    signature = signature * 0x100000001b3ull ^ image.info.guest_address ^
                                (u64(image.info.pixel_format) << 48) ^
                                (u64(image.info.tile_mode) << 40);
                    text += fmt::format(" [image {:#x} size {} {}x{} {} tile {} pitch {} bits {} samples {} "
                                        "layers {} mips {}{}{}]",
                                        image.info.guest_address, image.info.guest_size,
                                        image.info.size.width, image.info.size.height,
                                        vk::to_string(image.info.pixel_format),
                                        u32(image.info.tile_mode), image.info.pitch,
                                        image.info.num_bits, image.info.num_samples,
                                        image.info.resources.layers, image.info.resources.levels,
                                        True(image.flags & VideoCore::ImageFlagBits::GpuModified)
                                            ? " gpu-modified"
                                            : "",
                                        image.info.guest_address == sharp.base_address &&
                                                image.info.guest_size == sharp.GetSize()
                                            ? " exact"
                                            : "");
                });
        }
    }
    std::scoped_lock lk{mutex};
    if (seen.size() < 256 && seen.insert(signature).second) {
        // stdout: the GPU log hides Info unless BB_GPU_LOG=info.
        std::printf("RT kernel census: %s\n", text.c_str());
    }
}
#endif

#ifdef __APPLE__
// bbport: the SDK fill kernel 0x8b355b5a stores one float4 to every element of a buffer
// (fill_clear.h). When that buffer is exactly a cached render target and the stored bytes are
// one repeated 32-bit word, the kernel is replaced by a fill of those bytes (vkCmdFillBuffer over
// the whole range, padding included, so raw-buffer and CPU readers see what the kernel would have
// written) and a clear of the image to the same value (instead of the image's refresh from the
// buffer). Only where both are the same bits; otherwise the kernel runs, counted by reason.
// BB_RT_FILL_VERIFY=1 waits for each replaced fill and stops unless every byte of the range and
// texel (0, 0) of the image hold the bits the kernel would have stored.
namespace {
enum class FillPath : u32 {
    Cleared,
    Shape,
    Format,
    NoImage,
    NotExact,
    GpuValue,
    Value,
    HalvesDiffer,
    Reference,
    Count
};
constexpr std::array<const char*, u32(FillPath::Count)> fill_path_names = {
    "replaced",          "dispatch, SGPR or V# shape", "format",
    "no image",          "not exactly one image",      "value GPU-written or in an image",
    "value not exact",   "8-byte texel halves differ",     "kernel run as the verify reference"};
std::array<std::atomic<u64>, u32(FillPath::Count)> fill_counts{};

/// BB_RT_FILL_VERIFY: 1 checks every replaced fill; 2 also runs every other qualifying fill as the
/// kernel and checks the kernel's bytes against the same derived pattern.
const int fill_verify = [] {
    const char* env = std::getenv("BB_RT_FILL_VERIFY");
    return env ? std::atoi(env) : 0;
}();
/// A qualifying fill left to the kernel (verify 2), checked once its dispatch is recorded. Only the
/// thread recording dispatches touches it.
struct KernelCheck {
    bool armed;
    VAddr address;
    u64 size;
    u32 pattern;
};
KernelCheck kernel_check{};

/// Waits until the GPU ran everything recorded and stops unless the guest range holds `pattern`.
void CheckRangeHolds(Rasterizer& rasterizer, VAddr address, u64 size, u32 pattern,
                     const char* what) {
    rasterizer.GetRuntime().GetScheduler().Finish();
    const auto* bytes = reinterpret_cast<const u8*>(address);
    for (u64 offset = 0; offset < size; offset += 4) {
        u32 word;
        std::memcpy(&word, bytes + offset, 4);
        ASSERT_MSG(word == pattern,
                   "BB_RT_FILL_VERIFY: byte {:#x} of {:#x}+{:#x} holds {:#010x} after {}, the "
                   "derived fill is {:#010x}",
                   offset, address, size, word, what, pattern);
    }
}

void CountFill(FillPath path) {
    fill_counts[u32(path)].fetch_add(1, std::memory_order_relaxed);
    static std::atomic<u64> calls{0};
    if ((calls.fetch_add(1, std::memory_order_relaxed) + 1) % 4096 != 0) {
        return;
    }
    std::string text;
    for (u32 i = 0; i < u32(FillPath::Count); ++i) {
        text += fmt::format("{}{} {}", i ? ", " : "", fill_path_names[i],
                            fill_counts[i].load(std::memory_order_relaxed));
    }
    std::printf("RT fill (0x8b355b5a): %s\n", text.c_str());
}

vk::Format ImageFormat(FillClear::Format format) {
    switch (format) {
    case FillClear::Format::Rgba16Float:
        return vk::Format::eR16G16B16A16Sfloat;
    case FillClear::Format::B10G11R11Ufloat:
        return vk::Format::eB10G11R11UfloatPack32;
    case FillClear::Format::Rgba8Unorm:
        return vk::Format::eR8G8B8A8Unorm;
    }
    UNREACHABLE();
}

/// BB_RT_FILL_VERIFY=1: waits for the replaced fill and stops unless the whole guest range holds
/// `pattern` and texel (0, 0) of the image the texel the kernel stores.
void VerifyFillClear(Rasterizer& rasterizer, VideoCore::Image& image, VAddr address, u64 size,
                     u32 pattern, const std::array<u8, 8>& texel, u32 texel_bytes) {
    auto& runtime = rasterizer.GetRuntime();
    auto& scheduler = runtime.GetScheduler();
    const auto readback = runtime.GetStagingPool().Request(16, VideoCore::MemoryType::HostCached);
    runtime.Transit(&image, vk::ImageLayout::eTransferSrcOptimal,
                    vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead);
    runtime.FlushBarriers();
    const vk::BufferImageCopy region{
        .bufferOffset = readback.offset,
        .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .imageExtent = {1, 1, 1},
    };
    scheduler.Record([source = image.GetImage(), buffer = readback.buffer->Handle(),
                      region](vk::CommandBuffer cmdbuf) {
        cmdbuf.copyImageToBuffer(source, vk::ImageLayout::eTransferSrcOptimal, buffer, region);
    });
    scheduler.Finish();
    readback.Invalidate();
    ASSERT_MSG(std::memcmp(readback.mapped, texel.data(), texel_bytes) == 0,
               "BB_RT_FILL_VERIFY: the clear of image {:#x} ({}) wrote a texel other than the "
               "fill kernel's",
               image.info.guest_address, vk::to_string(image.info.pixel_format));
    const auto* bytes = reinterpret_cast<const u8*>(address);
    for (u64 offset = 0; offset < size; offset += 4) {
        u32 word;
        std::memcpy(&word, bytes + offset, 4);
        ASSERT_MSG(word == pattern,
                   "BB_RT_FILL_VERIFY: byte {:#x} of the fill of {:#x}+{:#x} holds {:#010x}, the "
                   "fill kernel stores {:#010x}",
                   offset, address, size, word, pattern);
    }
    static std::atomic<u64> verified{0};
    if (verified.fetch_add(1, std::memory_order_relaxed) % 256 == 0) {
        std::printf("BB_RT_FILL_VERIFY: %llu replaced fills verified (whole range and texel)\n",
                    (unsigned long long)verified.load(std::memory_order_relaxed));
    }
}

/// The fill as a buffer fill plus an image clear when that is exact; false: run the kernel.
bool FillAsImageClear(const Shader::Info& info, const AmdGpu::ComputeProgram& cs_program,
                      Rasterizer& rasterizer) {
    const auto fails = [](FillPath path) {
        CountFill(path);
        return false;
    };
    const Shader::BufferResource* value_desc = nullptr;
    const Shader::BufferResource* target_desc = nullptr;
    for (const auto& desc : info.buffers) {
        if (desc.IsSpecial()) {
            continue;
        }
        auto& slot = desc.is_written ? target_desc : value_desc;
        if (slot) {
            return fails(FillPath::Shape);
        }
        slot = &desc;
    }
    if (!value_desc || !target_desc) {
        return fails(FillPath::Shape);
    }
    const auto value_sharp = value_desc->GetSharp(info);
    const auto target = target_desc->GetSharp(info);
    const auto format = FillClear::TargetFormat(target.data_format, target.num_format);
    if (!format) {
        return fails(FillPath::Format);
    }
    const FillClear::Dispatch dispatch{
        .threads_x = cs_program.num_thread_x.full,
        .threads_x_partial = cs_program.num_thread_x.partial,
        .threads_y = cs_program.num_thread_y.full,
        .threads_y_partial = cs_program.num_thread_y.partial,
        .threads_z = cs_program.num_thread_z.full,
        .threads_z_partial = cs_program.num_thread_z.partial,
        .dim_x = cs_program.dim_x,
        .dim_y = cs_program.dim_y,
        .dim_z = cs_program.dim_z,
        .start_x = cs_program.start_x,
        .start_y = cs_program.start_y,
        .start_z = cs_program.start_z,
        .user_sgprs = cs_program.settings.num_user_regs,
        .tgid_enable = cs_program.settings.tgid_enable,
    };
    const FillClear::Target target_fields{
        .stride = u32(target.stride),
        .num_records = target.num_records,
        .swizzle_enable = target.swizzle_enable != 0,
        .add_tid_enable = target.add_tid_enable != 0,
        .dst_sel_x = target.dst_sel_x,
        .dst_sel_y = target.dst_sel_y,
        .dst_sel_z = target.dst_sel_z,
        .dst_sel_w = target.dst_sel_w,
    };
    const u32 texel_bytes = FillClear::TexelBytes(*format);
    if (!FillClear::CoversAllElements(dispatch, target_fields, texel_bytes) ||
        value_sharp.base_address == 0 || value_sharp.GetSize() < 16) {
        return fails(FillPath::Shape);
    }
    const VAddr address = target.base_address;
    const u64 size = u64(target.num_records) * texel_bytes;
    // Exactly one cached image, at exactly the buffer's range.
    auto& texture_cache = rasterizer.GetTextureCache();
    VideoCore::Image* image = nullptr;
    VideoCore::ImageId image_id{};
    u32 overlapping = 0;
    texture_cache.ForEachImageInRegion(address, size,
                                       [&](VideoCore::ImageId id, VideoCore::Image& candidate) {
                                           ++overlapping;
                                           image = &candidate;
                                           image_id = id;
                                       });
    if (overlapping == 0) {
        return fails(FillPath::NoImage);
    }
    // The texture binding helper finishes before image state is read or changed.
    auto& runtime = rasterizer.GetRuntime();
    runtime.BeforeImageAccess();
    const auto& image_info = image->info;
    if (overlapping != 1 || image_info.guest_address != address ||
        image_info.guest_size != size || image_info.resources.levels != 1 ||
        image_info.resources.layers != 1 || image_info.props.is_depth ||
        image_info.props.is_volume || image_info.size.depth != 1 ||
        image->backing->num_samples != 1) {
        return fails(FillPath::NotExact);
    }
    if (image_info.pixel_format != ImageFormat(*format)) {
        return fails(FillPath::Format);
    }
    // The value the kernel loads: its guest bytes, unless a GPU write to them (through a buffer
    // or an aliased image) is outstanding.
    auto& buffer_cache = rasterizer.GetBufferCache();
    bool value_in_image = false;
    texture_cache.ForEachImageInRegion(value_sharp.base_address, 16,
                                       [&](VideoCore::ImageId, VideoCore::Image&) {
                                           value_in_image = true;
                                       });
    if (value_in_image || buffer_cache.IsRegionGpuModified(value_sharp.base_address, 16)) {
        return fails(FillPath::GpuValue);
    }
    std::array<float, 4> value;
    std::memcpy(value.data(), reinterpret_cast<const void*>(value_sharp.base_address), 16);
    const auto texel = FillClear::Texel(*format, value);
    if (!texel) {
        return fails(FillPath::Value);
    }
    const auto pattern = FillClear::Pattern(*format, *texel);
    if (!pattern) {
        return fails(FillPath::HalvesDiffer);
    }
    // BB_RT_FILL_VERIFY=2: every other qualifying fill runs the kernel itself, and AfterDispatchHLE
    // checks that it wrote exactly the derived bytes: the reference for the replaced ones.
    static u64 qualifying = 0;
    if (fill_verify == 2 && (qualifying++ & 1) == 0) {
        kernel_check = {true, address, size, *pattern};
        return fails(FillPath::Reference);
    }
    // The bytes, as the kernel writes them (the buffer write it would have bound).
    const auto [buffer, offset] = buffer_cache.ObtainBuffer(address, u32(size), true, false);
    ASSERT_MSG(offset % 4 == 0, "RT fill: target {:#x} is not 4-byte aligned in its buffer",
               address);
    runtime.FillBuffer(buffer, offset, size, *pattern);
    // The image, as its refresh from those bytes would leave it.
    vk::ClearValue clear{};
    clear.color.float32 = value;
    runtime.ClearImage(image, VideoCore::SubresourceRange{}, clear);
    // ClearImage leaves it GPU-modified and clean, i.e. trusted for write-backs: it must notice
    // guest CPU writes from now on (the kernel path instead leaves it GPU-dirty, refreshed from
    // memory at its next use).
    texture_cache.TrackGpuWritten(image_id);
    CountFill(FillPath::Cleared);
    if (fill_verify) {
        VerifyFillClear(rasterizer, *image, address, size, *pattern, *texel, texel_bytes);
    }
    return true;
}
} // namespace

void AfterDispatchHLE(Rasterizer& rasterizer) {
    if (!kernel_check.armed) {
        return;
    }
    kernel_check.armed = false;
    CheckRangeHolds(rasterizer, kernel_check.address, kernel_check.size, kernel_check.pattern,
                    "the fill kernel");
    static u64 matched = 0;
    if (matched++ % 256 == 0) {
        std::printf("BB_RT_FILL_VERIFY=2: %llu kernel fills wrote exactly the derived bytes\n",
                    (unsigned long long)matched);
    }
}
#endif

bool ExecuteShaderHLE(const Shader::Info& info, const AmdGpu::Regs& regs,
                      const AmdGpu::ComputeProgram& cs_program, Rasterizer& rasterizer) {
#ifdef __APPLE__
    if (kernel_check.armed) {
        // The previous dispatch was not recorded (its bindings failed): nothing to check.
        std::printf("BB_RT_FILL_VERIFY=2: a reference fill was not dispatched, not checked\n");
        kernel_check.armed = false;
    }
    if (info.pgm_hash == SDK_FILL_HASH || info.pgm_hash == SDK_COPY_HASH) {
        CensusRenderTargetKernel(info, cs_program, rasterizer);
    }
    if (info.pgm_hash == SDK_FILL_HASH && !BbToggle::Disabled(BbToggle::RtFillKernel) &&
        FillAsImageClear(info, cs_program, rasterizer)) {
        return true;
    }
#endif
    switch (info.pgm_hash) {
    case COPY_SHADER_HASH:
        return ExecuteCopyShaderHLE(info, cs_program, rasterizer);
    default:
        return false;
    }
}

} // namespace Vulkan
