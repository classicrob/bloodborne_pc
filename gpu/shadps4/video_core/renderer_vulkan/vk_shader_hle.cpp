// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_set>
#include "shader_recompiler/info.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
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
                    text += fmt::format(" [image {:#x} size {} {}x{} {} tile {} layers {} mips {}{}]",
                                        image.info.guest_address, image.info.guest_size,
                                        image.info.size.width, image.info.size.height,
                                        vk::to_string(image.info.pixel_format),
                                        u32(image.info.tile_mode), image.info.resources.layers,
                                        image.info.resources.levels,
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
// bbport: the SDK fill kernel 0x8b355b5a stores one float4 to every element of a buffer. Its code
// (Opus-GS disassembly, out/shader_sweep/disasm-renderer/0x8b355b5a.s): S_BUFFER_LOAD_DWORDX4 of
// the V# at s[8:11] (offset 0), then BUFFER_STORE_FORMAT_XYZW (typed: converted by the V#'s
// format) to the V# at s[4:7], element g*128 + (t & 63) and that +64, for group g and thread t
// of 64. When that buffer is exactly a cached render target, the fill is a clear of the image:
// the buffer write and the image's refresh from it are skipped. Only where the clear gives the
// same bits (FillTexel: every component exactly representable in the format, so no rounding is
// involved); otherwise the kernel runs, counted by reason. BB_RT_FILL_VERIFY=1 reads a cleared
// texel back and stops on any difference.
namespace {
enum class FillPath : u32 { Cleared, Shape, NoImage, NotExact, Format, GpuValue, Value, Count };
constexpr std::array<const char*, u32(FillPath::Count)> fill_path_names = {
    "cleared", "dispatch or V# shape", "no image", "not exactly one image", "format",
    "value GPU-written", "value not exact"};
std::array<std::atomic<u64>, u32(FillPath::Count)> fill_counts{};

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

/// A float exactly representable as a small float with a 5-bit exponent (bias 15): its bits, or
/// nothing (rounding, denormal, out of range, Inf/NaN, -0, or a sign the format has not).
std::optional<u32> SmallFloatBits(float value, u32 mantissa_bits, bool has_sign) {
    const u32 bits = std::bit_cast<u32>(value);
    if (bits == 0) {
        return 0u;
    }
    const u32 sign = bits >> 31;
    const s32 exponent = s32((bits >> 23) & 0xff) - 127;
    const u32 mantissa = bits & 0x7fffff;
    if ((sign && !has_sign) || exponent < -14 || exponent > 15 ||
        (mantissa & ((1u << (23 - mantissa_bits)) - 1)) != 0) {
        return std::nullopt;
    }
    return (sign << (5 + mantissa_bits)) | (u32(exponent + 15) << mantissa_bits) |
           (mantissa >> (23 - mantissa_bits));
}

/// A float in [0, 1] whose unorm8 value needs no rounding: v * 255 is an integer.
std::optional<u32> Unorm8Bits(float value) {
    if (!(value >= 0.0f && value <= 1.0f) || std::bit_cast<u32>(value) == 0x80000000u) {
        return std::nullopt;
    }
    const double scaled = double(value) * 255.0;
    if (scaled != std::floor(scaled)) {
        return std::nullopt;
    }
    return u32(scaled);
}

struct FillFormat {
    AmdGpu::DataFormat data_format;
    AmdGpu::NumberFormat number_format;
    vk::Format image_format;
    u32 texel_bytes;
};
constexpr std::array<FillFormat, 3> fill_formats = {{
    {AmdGpu::DataFormat::Format16_16_16_16, AmdGpu::NumberFormat::Float,
     vk::Format::eR16G16B16A16Sfloat, 8},
    {AmdGpu::DataFormat::Format10_11_11, AmdGpu::NumberFormat::Float,
     vk::Format::eB10G11R11UfloatPack32, 4},
    {AmdGpu::DataFormat::Format8_8_8_8, AmdGpu::NumberFormat::Unorm, vk::Format::eR8G8B8A8Unorm,
     4},
}};

/// The texel the typed store writes for `value`, when no rounding is involved.
std::optional<std::array<u8, 8>> FillTexel(vk::Format format, const std::array<float, 4>& value) {
    std::array<u8, 8> texel{};
    if (format == vk::Format::eR16G16B16A16Sfloat) {
        for (u32 c = 0; c < 4; ++c) {
            const auto half = SmallFloatBits(value[c], 10, true);
            if (!half) {
                return std::nullopt;
            }
            std::memcpy(texel.data() + c * 2, &*half, 2);
        }
        return texel;
    }
    if (format == vk::Format::eB10G11R11UfloatPack32) {
        // x: bits 0-10, y: 11-21, z: 22-31 (10_11_11); w is not stored.
        const auto r = SmallFloatBits(value[0], 6, false);
        const auto g = SmallFloatBits(value[1], 6, false);
        const auto b = SmallFloatBits(value[2], 5, false);
        if (!r || !g || !b) {
            return std::nullopt;
        }
        const u32 packed = *r | (*g << 11) | (*b << 22);
        std::memcpy(texel.data(), &packed, 4);
        return texel;
    }
    for (u32 c = 0; c < 4; ++c) { // eR8G8B8A8Unorm
        const auto unorm = Unorm8Bits(value[c]);
        if (!unorm) {
            return std::nullopt;
        }
        texel[c] = u8(*unorm);
    }
    return texel;
}

/// BB_RT_FILL_VERIFY=1: reads texel (0, 0) of the cleared image back and stops unless it holds
/// the bits the kernel would have stored.
void VerifyFillClear(Rasterizer& rasterizer, VideoCore::Image& image,
                     const std::array<u8, 8>& texel, u32 texel_bytes) {
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
    static std::atomic<u64> verified{0};
    ASSERT_MSG(std::memcmp(readback.mapped, texel.data(), texel_bytes) == 0,
               "BB_RT_FILL_VERIFY: the clear of image {:#x} ({}) wrote texel {:02x} where the "
               "fill kernel stores {:02x}",
               image.info.guest_address, vk::to_string(image.info.pixel_format),
               fmt::join(std::span{readback.mapped, texel_bytes}, " "),
               fmt::join(std::span{texel.data(), texel_bytes}, " "));
    if (verified.fetch_add(1, std::memory_order_relaxed) % 256 == 0) {
        std::printf("BB_RT_FILL_VERIFY: %llu fill clears read back identical\n",
                    (unsigned long long)verified.load(std::memory_order_relaxed));
    }
}

/// The fill as an image clear when that is exact; false: run the kernel.
bool FillAsImageClear(const Shader::Info& info, const AmdGpu::ComputeProgram& cs_program,
                      Rasterizer& rasterizer) {
    const auto shape_fails = [&] {
        CountFill(FillPath::Shape);
        return false;
    };
    if (cs_program.num_thread_x.full != 64 || cs_program.num_thread_x.partial != 0 ||
        cs_program.num_thread_y.full != 1 || cs_program.num_thread_z.full != 1 ||
        cs_program.dim_y != 1 || cs_program.dim_z != 1 || cs_program.start_x != 0 ||
        cs_program.start_y != 0 || cs_program.start_z != 0) {
        return shape_fails();
    }
    const Shader::BufferResource* value_desc = nullptr;
    const Shader::BufferResource* target_desc = nullptr;
    for (const auto& desc : info.buffers) {
        if (desc.IsSpecial()) {
            continue;
        }
        auto& slot = desc.is_written ? target_desc : value_desc;
        if (slot) {
            return shape_fails();
        }
        slot = &desc;
    }
    if (!value_desc || !target_desc) {
        return shape_fails();
    }
    const auto value_sharp = value_desc->GetSharp(info);
    const auto target = target_desc->GetSharp(info);
    const auto format_it = std::ranges::find_if(fill_formats, [&](const FillFormat& f) {
        return f.data_format == target.GetDataFmt() && f.number_format == target.GetNumberFmt();
    });
    if (format_it == fill_formats.end()) {
        CountFill(FillPath::Format);
        return false;
    }
    const u64 size = u64(target.num_records) * target.stride;
    if (target.swizzle_enable || target.add_tid_enable || target.stride != format_it->texel_bytes ||
        target.dst_sel_x != 4 || target.dst_sel_y != 5 || target.dst_sel_z != 6 ||
        target.dst_sel_w != 7 || size == 0 || u64(cs_program.dim_x) * 128 < target.num_records ||
        value_sharp.base_address == 0 || value_sharp.GetSize() < 16) {
        return shape_fails();
    }
    // Exactly one cached image, at exactly the buffer's range: the fill covers all of it.
    auto& texture_cache = rasterizer.GetTextureCache();
    VideoCore::Image* image = nullptr;
    u32 overlapping = 0;
    texture_cache.ForEachImageInRegion(target.base_address, size,
                                       [&](VideoCore::ImageId, VideoCore::Image& candidate) {
                                           ++overlapping;
                                           image = &candidate;
                                       });
    if (overlapping == 0) {
        CountFill(FillPath::NoImage);
        return false;
    }
    // The texture binding helper finishes before image state is read or changed.
    rasterizer.GetRuntime().BeforeImageAccess();
    const auto& image_info = image->info;
    if (overlapping != 1 || image_info.guest_address != target.base_address ||
        image_info.guest_size != size || image_info.resources.levels != 1 ||
        image_info.resources.layers != 1 || image_info.props.is_depth ||
        image_info.props.is_volume || image_info.size.depth != 1 ||
        image->backing->num_samples != 1) {
        CountFill(FillPath::NotExact);
        return false;
    }
    if (image_info.pixel_format != format_it->image_format) {
        CountFill(FillPath::Format);
        return false;
    }
    // The value the kernel loads, as the GPU would read it: not while a GPU write to it is
    // outstanding.
    if (rasterizer.GetBufferCache().IsRegionGpuModified(value_sharp.base_address, 16)) {
        CountFill(FillPath::GpuValue);
        return false;
    }
    std::array<float, 4> value;
    std::memcpy(value.data(), reinterpret_cast<const void*>(value_sharp.base_address), 16);
    const auto texel = FillTexel(image_info.pixel_format, value);
    if (!texel) {
        CountFill(FillPath::Value);
        return false;
    }
    vk::ClearValue clear{};
    clear.color.float32 = value;
    rasterizer.GetRuntime().ClearImage(image, VideoCore::SubresourceRange{}, clear);
    CountFill(FillPath::Cleared);
    static const bool verify = [] {
        const char* env = std::getenv("BB_RT_FILL_VERIFY");
        return env && env[0] == '1';
    }();
    if (verify) {
        VerifyFillClear(rasterizer, *image, *texel, format_it->texel_bytes);
    }
    return true;
}
} // namespace
#endif

bool ExecuteShaderHLE(const Shader::Info& info, const AmdGpu::Regs& regs,
                      const AmdGpu::ComputeProgram& cs_program, Rasterizer& rasterizer) {
#ifdef __APPLE__
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
