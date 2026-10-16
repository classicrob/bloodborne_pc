#if defined(BC4_FLOAT_MIP7_BUFFER_PROBE) && !defined(BC4_FLOAT_MIP7_PROBE)
#error "BC4_FLOAT_MIP7_BUFFER_PROBE requires BC4_FLOAT_MIP7_PROBE"
#endif
#if defined(BC4_FLOAT_MIP7_PROBE)
// Direct compressed-tail/float capability and a selective CPU publication model only.
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

namespace {
using Word = std::uint32_t;
constexpr unsigned Levels = 9, TargetMip = 7, Guard = 32;
constexpr unsigned GuestBytes = 0x6000, TailOffset = 0x5c00, TailBytes = 512;
constexpr unsigned MaxMipBytes = 256 * 128 / 2, Stride = MaxMipBytes + 2 * Guard;
constexpr unsigned Slots = 2 * Levels + 2, HostBytes = Slots * Stride;
constexpr unsigned FloatUpload = 2 * Levels, FloatRead = FloatUpload + 1;
constexpr std::array<Word, 2> Initial{0x3f800000, 0xc0000000}; // 1, -2
constexpr std::array<Word, 2> Poison{0xc0800000, 0xc1000000};  // -4, -8
constexpr std::array<Word, 2> Written{0x3f000000, 0x40400000}; // 0.5, 3
static_assert(std::bit_cast<float>(Written[0]) == 0.5f &&
              std::bit_cast<float>(Written[1]) == 3.0f);
constexpr unsigned Width(unsigned mip) { return std::max(256u >> mip, 1u); }
constexpr unsigned Height(unsigned mip) { return std::max(128u >> mip, 1u); }
constexpr unsigned MipBytes(unsigned mip) {
    return ((Width(mip) + 3) / 4) * ((Height(mip) + 3) / 4) * 8;
}
constexpr unsigned Offset(unsigned slot) { return slot * Stride + Guard; }
constexpr unsigned SlotBytes(unsigned slot) {
    return slot < 2 * Levels ? MipBytes(slot % Levels) : 8;
}
std::vector<Word> Seed(unsigned mip) {
    std::vector<Word> words(MipBytes(mip) / 4);
    for (unsigned i = 0; i < words.size(); ++i)
        words[i] = 0x3f000000u + mip * 0x10000u + i;
    if (mip == TargetMip) std::copy(Initial.begin(), Initial.end(), words.begin());
    return words;
}
using Guest = std::array<unsigned char, GuestBytes + 2 * Guard>;
Guest GuestSeed() {
    Guest bytes{};
    bytes.fill(0xa5);
    for (unsigned i = 0; i < GuestBytes; ++i) bytes[Guard + i] = (i * 37 + 11) & 255;
    std::memcpy(bytes.data() + Guard + TailOffset, Initial.data(), 8);
    return bytes;
}
bool PublicationMatches(const Guest& actual) {
    auto expected = GuestSeed();
    std::memcpy(expected.data() + Guard + TailOffset, Written.data(), 8);
    return actual == expected;
}
bool HostCanaries(const unsigned char* data) {
    for (unsigned slot = 0; slot < Slots; ++slot) {
        for (unsigned i = 0; i < Stride; ++i) {
            if (i >= Guard && i < Guard + SlotBytes(slot)) continue;
            if (data[slot * Stride + i] != 0xa5) return false;
        }
    }
    return true;
}
#ifdef BC4_FLOAT_MIP7_BUFFER_PROBE
constexpr unsigned BridgeOffset = Guard, BridgeBytes = 2 * Guard + 8;
static_assert(BridgeOffset >= 8 && BridgeOffset % 8 == 0);
bool BridgeCanaries(const unsigned char* data) {
    for (unsigned i = 0; i < BridgeBytes; ++i) {
        if (i >= BridgeOffset && i < BridgeOffset + 8) continue;
        if (data[i] != 0xa5) return false;
    }
    return true;
}
bool BridgeOracleControls() {
    std::array<unsigned char, BridgeBytes> bytes;
    bytes.fill(0xa5);
    for (const auto& expected : {Initial, Written}) {
        const std::array<Word, 2> poison{~expected[0], ~expected[1]};
        std::memcpy(bytes.data() + BridgeOffset, poison.data(), 8);
        if (!std::memcmp(bytes.data() + BridgeOffset, expected.data(), 8) ||
            !BridgeCanaries(bytes.data())) return false;
    }
    for (unsigned i = 0; i < BridgeBytes; ++i) {
        if (i >= BridgeOffset && i < BridgeOffset + 8) continue;
        bytes[i] ^= 1;
        if (BridgeCanaries(bytes.data())) return false;
        bytes[i] ^= 1;
    }
    return true;
}
#endif
bool OracleControls() {
    if (MipBytes(TargetMip) != 8 || Width(TargetMip) != 2 || Height(TargetMip) != 1)
        return false;
    auto correct = GuestSeed();
    if (PublicationMatches(correct)) return false;
    std::memcpy(correct.data() + Guard + TailOffset, Written.data(), 8);
    if (!PublicationMatches(correct)) return false;
    for (unsigned i = 0; i < correct.size(); ++i) {
        correct[i] ^= 1;
        if (PublicationMatches(correct)) return false;
        correct[i] ^= 1;
    }
    auto broad = GuestSeed();
    std::memset(broad.data() + Guard + TailOffset, 0xcc, TailBytes);
    std::memcpy(broad.data() + Guard + TailOffset, Written.data(), 8);
    if (PublicationMatches(broad)) return false;
    for (unsigned mip = 0; mip < Levels; ++mip) {
        const auto expected = Seed(mip);
        auto unread = expected;
        for (auto& word : unread) word = ~word;
        if (!std::memcmp(unread.data(), expected.data(), MipBytes(mip))) return false;
    }
#ifdef BC4_FLOAT_MIP7_BUFFER_PROBE
    if (!BridgeOracleControls()) return false;
#endif
    return Initial != Poison && Initial != Written && Poison != Written;
}
} // namespace

#ifdef BC4_COPY_ORACLE_ONLY
int main() {
    const bool ok = OracleControls();
#ifdef BC4_FLOAT_MIP7_BUFFER_PROBE
    std::printf("bc4-float-mip7-buffer CPU oracle: %s (no Vulkan execution)\n", ok ? "PASS" : "FAIL");
#else
    std::printf("bc4-float-mip7 CPU oracle: %s (no Vulkan execution)\n", ok ? "PASS" : "FAIL");
#endif
    return ok ? 0 : 1;
}
#else
#include <sirit/sirit.h>
#include "test_alarm.h"
#include "common/slot_vector.h"
#include "shader_recompiler/resource.h"
#include "video_core/amdgpu/resource.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/blit_helper.h"
#include "video_core/texture_cache/image.h"
#include <vk_mem_alloc.h>

namespace {
unsigned failures = 0;
void Expect(const char* name, bool ok) {
    std::printf("%-72s %s\n", name, ok ? "ok" : "FAIL");
    failures += !ok;
}
template <typename T> T Require(vk::ResultValue<T> value, const char* name) {
    if (value.result != vk::Result::eSuccess) {
        std::fprintf(stderr, "STOP: %s Vulkan result%d; no fallback\n", name, int(value.result));
        std::exit(2);
    }
    return std::move(value.value);
}
VideoCore::ImageInfo FloatTailInfo(bool bc) {
    auto sharp = AmdGpu::Image::Null(false);
    sharp.base_address = bc ? 0x10000 : 0x20000;
    sharp.data_format = u64(bc ? AmdGpu::DataFormat::FormatBc4 : AmdGpu::DataFormat::Format32_32);
    sharp.num_format = u64(bc ? AmdGpu::NumberFormat::Unorm : AmdGpu::NumberFormat::Float);
    sharp.width = bc ? 255 : 0;
    sharp.height = bc ? 127 : 0;
    sharp.pitch = bc ? 255 : 7;
    sharp.last_level = bc ? 8 : 0;
    sharp.tiling_index = 13;
    sharp.dst_sel_x = 4;
    sharp.dst_sel_y = 5;
    sharp.dst_sel_z = 0;
    sharp.dst_sel_w = 1;
    Shader::ImageResource resource{};
    resource.is_written = !bc;
    resource.is_array = true;
    return {sharp, resource};
}
std::vector<u32> FloatStoreShader() {
    Sirit::Module m(0x00010500);
    m.AddCapability(spv::Capability::Shader);
    m.AddCapability(spv::Capability::StorageImageExtendedFormats);
    const auto u = m.TypeUInt(32), f = m.TypeFloat(32);
    const auto v2 = m.TypeVector(u, 2), v4 = m.TypeVector(f, 4);
    const auto image_type = m.TypeImage(f, spv::Dim::Dim2D, 0, false, false, 2,
                                      spv::ImageFormat::Rg32f);
    const auto image = m.AddGlobalVariable(m.TypePointer(spv::StorageClass::UniformConstant, image_type),
                                          spv::StorageClass::UniformConstant);
    m.Decorate(image, spv::Decoration::DescriptorSet, 0u);
    m.Decorate(image, spv::Decoration::Binding, 0u);
    m.Decorate(image, spv::Decoration::NonReadable);
    const auto fn = m.OpFunction(m.TypeVoid(), spv::FunctionControlMask::MaskNone,
                                 m.TypeFunction(m.TypeVoid()));
    m.AddLabel(m.OpLabel());
    m.OpImageWrite(m.OpLoad(image_type, image),
                  m.ConstantComposite(v2, m.Constant(u, 0u), m.Constant(u, 0u)),
                  m.ConstantComposite(v4, m.Constant(f, 0.5f), m.Constant(f, 3.0f),
                                      m.Constant(f, 0.0f), m.Constant(f, 1.0f)));
    m.OpReturn();
    m.OpFunctionEnd();
    const std::array interfaces{image};
    m.AddEntryPoint(spv::ExecutionModel::GLCompute, fn, "main", interfaces);
    m.AddExecutionMode(fn, spv::ExecutionMode::LocalSize, 1u, 1u, 1u);
    m.SetMemoryModel(spv::AddressingModel::Logical, spv::MemoryModel::GLSL450);
    return m.Assemble();
}
} // namespace

int main(int argc, char** argv) {
    const char* mode = argc == 2 ? argv[1] : "none";
#ifdef BC4_FLOAT_MIP7_BUFFER_PROBE
    const std::array allowed{"none", "--skip-forward", "--skip-store", "--skip-reverse",
                             "--skip-publication", "--overpublish", "--skip-sibling-readback",
                             "--skip-forward-upload", "--skip-reverse-upload"};
#else
    const std::array allowed{"none", "--skip-forward", "--skip-store", "--skip-reverse",
                             "--skip-publication", "--overpublish", "--skip-sibling-readback"};
#endif
    if (argc > 2 || std::none_of(allowed.begin(), allowed.end(),
                                [&](const char* s) { return !std::strcmp(s, mode); })) {
#ifdef BC4_FLOAT_MIP7_BUFFER_PROBE
        std::fputs("usage: bc4-float-mip7-buffer-test [--skip-forward|--skip-forward-upload|"
                   "--skip-store|--skip-reverse|--skip-reverse-upload|--skip-publication|"
                   "--overpublish|--skip-sibling-readback]\n", stderr);
#else
        std::fputs("usage: bc4-float-mip7-test [--skip-forward|--skip-store|--skip-reverse|"
                   "--skip-publication|--overpublish|--skip-sibling-readback]\n", stderr);
#endif
        return 2;
    }
    const auto is = [&](const char* s) { return !std::strcmp(s, mode); };
    test_alarm(120);
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    Expect("CPU full-byte publication and corruption oracles", OracleControls());
    if (failures) return 1;
    Vulkan::Instance instance(0, false);
    vk::detail::DynamicLoader loader;
    vk::detail::DispatchLoaderDynamic d;
    d.init(loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr"));
    d.init(instance.GetInstance());
    d.init(instance.GetDevice());
    const auto device = instance.GetDevice();
    const auto properties = instance.GetPhysicalDevice().getProperties(d);
    std::printf("device=%s vendor=%#x driver=%#x mutation=%s\n", properties.deviceName.data(),
                properties.vendorID, properties.driverVersion, mode);
#ifdef BC4_FLOAT_MIP7_BUFFER_PROBE
    std::puts("Explicit image-buffer-image capability; guarded8-byte bridge, no direct copy or fallback; "
              "finite Rg32f constants; no live shader conversion, arbitrary-NaN, TextureCache or VM proof");
#else
    std::puts("Direct vkCmdCopyImage capability; finite Rg32f constants; no Runtime::CopyImage, "
              "live shader conversion, arbitrary-NaN, TextureCache or VM proof");
#endif
    const auto transfer_flags = vk::FormatFeatureFlagBits2::eTransferSrc |
                                vk::FormatFeatureFlagBits2::eTransferDst;
    if (!instance.IsFormatSupported(vk::Format::eBc4UnormBlock, transfer_flags) ||
        !instance.IsFormatSupported(vk::Format::eR32G32Sfloat,
                                    transfer_flags | vk::FormatFeatureFlagBits2::eStorageImage) ||
        !instance.GetPhysicalDevice().getFeatures(d).shaderStorageImageExtendedFormats) {
        std::fputs("STOP: BC4/RG32Float transfer-storage capability unsupported; no fallback\n", stderr);
        return 2;
    }
    Vulkan::Scheduler scheduler(instance);
    Vulkan::Runtime runtime(instance, scheduler);
    scheduler.SetSubmitCallback([&](Vulkan::SubmitInfo&) { runtime.FlushBarriers(); });
    Common::SlotVector<VideoCore::ImageView> views;
    const auto parent_info = FloatTailInfo(true), float_info = FloatTailInfo(false);
    VideoCore::Image parent(instance, runtime, views, parent_info);
    VideoCore::Image companion(instance, runtime, views, float_info);
    Expect("real parent geometry and selected tile13 allocation", parent_info.resources.levels == Levels &&
        parent_info.size.width == 256 && parent_info.size.height == 128 && parent_info.num_samples == 1 &&
        parent_info.resources.layers == 1 && parent_info.num_bits == 64 && parent_info.props.is_block &&
        parent_info.tile_mode == AmdGpu::TileMode::Thin1DThin &&
        parent_info.array_mode == AmdGpu::ArrayMode::Array1DTiledThin1 &&
        parent_info.mips_layout[TargetMip].offset == TailOffset &&
        parent_info.mips_layout[TargetMip].size == TailBytes && parent_info.guest_size == GuestBytes);
    Expect("real typed companion has one visible texel and512-byte tile allocation",
        float_info.size.width == 1 && float_info.size.height == 1 && float_info.pitch == 8 &&
        float_info.resources.levels == 1 && float_info.resources.layers == 1 && float_info.num_samples == 1 &&
        float_info.num_bits == 64 && !float_info.props.is_block && float_info.guest_size == TailBytes &&
        float_info.tile_mode == AmdGpu::TileMode::Thin1DThin &&
        float_info.array_mode == AmdGpu::ArrayMode::Array1DTiledThin1);
    Expect("actual backing formats/mips and typed storage usage",
        parent.backing->image.image_ci.format == vk::Format::eBc4UnormBlock &&
        parent.backing->image.image_ci.mipLevels == Levels &&
        companion.backing->image.image_ci.format == vk::Format::eR32G32Sfloat &&
        companion.backing->image.image_ci.mipLevels == 1 &&
        bool(companion.usage_flags & vk::ImageUsageFlagBits::eStorage));
    if (failures) { std::puts("SETUP FAIL before mutation"); scheduler.Close(); return 1; }

    VkBuffer host{};
    VmaAllocation allocation{};
    VmaAllocationInfo mapped{};
    const VkBufferCreateInfo bi{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = HostBytes,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    const VmaAllocationCreateInfo ac{.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT, .usage = VMA_MEMORY_USAGE_AUTO,
        .requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT};
    if (vmaCreateBuffer(instance.GetAllocator(), &bi, &ac, &host, &allocation, &mapped) != VK_SUCCESS) {
        std::fputs("STOP: host allocation failed; no fallback\n", stderr);
        scheduler.Close(); return 2;
    }
#ifdef BC4_FLOAT_MIP7_BUFFER_PROBE
    VkBuffer bridge{};
    VmaAllocation bridge_allocation{};
    VmaAllocationInfo bridge_mapped{};
    auto bridge_info = bi;
    bridge_info.size = BridgeBytes;
    if (vmaCreateBuffer(instance.GetAllocator(), &bridge_info, &ac, &bridge,
                        &bridge_allocation, &bridge_mapped) != VK_SUCCESS) {
        std::fputs("STOP: dedicated bridge allocation failed; no fallback\n", stderr);
        vmaDestroyBuffer(instance.GetAllocator(), host, allocation);
        scheduler.Close(); return 2;
    }
    auto* bridge_data = static_cast<unsigned char*>(bridge_mapped.pMappedData);
    std::memset(bridge_data, 0xa5, BridgeBytes);
    unsigned forward_upload_calls = 0, reverse_upload_calls = 0;
#endif
    auto* data = static_cast<unsigned char*>(mapped.pMappedData);
    std::memset(data, 0xa5, HostBytes);
    for (unsigned mip = 0; mip < Levels; ++mip) {
        const auto seed = Seed(mip);
        std::memcpy(data + Offset(mip), seed.data(), MipBytes(mip));
    }
    std::memcpy(data + Offset(FloatUpload), Poison.data(), 8);
    vmaFlushAllocation(instance.GetAllocator(), allocation, 0, VK_WHOLE_SIZE);
    unsigned forward_calls = 0, reverse_calls = 0, store_calls = 0;
    const auto finish = [&] {
        runtime.FlushBarriers();
        scheduler.Record([&](vk::CommandBuffer cmd) {
            const vk::MemoryBarrier2 barrier{.srcStageMask = vk::PipelineStageFlagBits2::eCopy,
                .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eHost,
                .dstAccessMask = vk::AccessFlagBits2::eHostRead};
            cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &barrier}, d);
        });
        scheduler.Finish();
        vmaInvalidateAllocation(instance.GetAllocator(), allocation, 0, VK_WHOLE_SIZE);
#ifdef BC4_FLOAT_MIP7_BUFFER_PROBE
        vmaInvalidateAllocation(instance.GetAllocator(), bridge_allocation, 0, VK_WHOLE_SIZE);
#endif
    };
    const auto shutdown = [&] {
        scheduler.Finish();
        scheduler.Close();
#ifdef BC4_FLOAT_MIP7_BUFFER_PROBE
        vmaDestroyBuffer(instance.GetAllocator(), bridge, bridge_allocation);
#endif
        vmaDestroyBuffer(instance.GetAllocator(), host, allocation);
        test_alarm(0);
#ifdef BC4_FLOAT_MIP7_BUFFER_PROBE
        std::printf("buffer-copy calls forward-source=%u forward-upload=%u reverse-source=%u "
                    "reverse-upload=%u typed-store=%u; direct-copy=0; no fallback\n",
                    forward_calls, forward_upload_calls, reverse_calls, reverse_upload_calls, store_calls);
        std::printf("bc4-float-mip7-buffer-test: %s (%u failures)\n", failures ? "FAIL" : "PASS", failures);
#else
        std::printf("direct-copy calls forward=%u reverse=%u typed-store=%u; no fallback\n",
                    forward_calls, reverse_calls, store_calls);
        std::printf("bc4-float-mip7-test: %s (%u failures)\n", failures ? "FAIL" : "PASS", failures);
#endif
        return failures ? 1 : 0;
    };
    const auto transfer = [&](VideoCore::Image& image, unsigned mip, unsigned slot, bool upload) {
        const auto layout = upload ? vk::ImageLayout::eTransferDstOptimal : vk::ImageLayout::eTransferSrcOptimal;
        runtime.Transit(&image, layout, vk::PipelineStageFlagBits2::eCopy,
                        upload ? vk::AccessFlagBits2::eTransferWrite : vk::AccessFlagBits2::eTransferRead);
        runtime.FlushBarriers();
        const vk::BufferImageCopy region{.bufferOffset = Offset(slot),
            .imageSubresource = {vk::ImageAspectFlagBits::eColor, mip, 0, 1},
            .imageExtent = {std::max(image.info.size.width >> mip, 1u),
                            std::max(image.info.size.height >> mip, 1u), 1}};
        scheduler.Record([&, handle = image.GetImage(), layout, region, upload](vk::CommandBuffer cmd) {
            if (upload) cmd.copyBufferToImage(host, handle, layout, region, d);
            else cmd.copyImageToBuffer(handle, layout, host, region, d);
        });
    };
    // Every host poison write occurs after the preceding Finish, before this phase records.
    const auto poison_parent = [&](bool changed) {
        for (unsigned mip = 0; mip < Levels; ++mip) {
            auto poison = Seed(mip);
            if (changed && mip == TargetMip) std::copy(Written.begin(), Written.end(), poison.begin());
            for (auto& word : poison) word = ~word;
            std::memcpy(data + Offset(Levels + mip), poison.data(), MipBytes(mip));
        }
    };
    const auto poison_float = [&](const std::array<Word, 2>& expected) {
        const std::array<Word, 2> poison{~expected[0], ~expected[1]};
        std::memcpy(data + Offset(FloatRead), poison.data(), 8);
    };
    const auto flush_host = [&] {
        vmaFlushAllocation(instance.GetAllocator(), allocation, 0, VK_WHOLE_SIZE);
#ifdef BC4_FLOAT_MIP7_BUFFER_PROBE
        vmaFlushAllocation(instance.GetAllocator(), bridge_allocation, 0, VK_WHOLE_SIZE);
#endif
    };
    const auto read_parent = [&](bool skip_sibling = false) {
        for (unsigned mip = 0; mip < Levels; ++mip) {
            if (skip_sibling && mip == 0) continue;
            transfer(parent, mip, Levels + mip, false);
        }
    };
    const auto parent_matches = [&](bool changed) {
        for (unsigned mip = 0; mip < Levels; ++mip) {
            auto expected = Seed(mip);
            if (changed && mip == TargetMip) std::copy(Written.begin(), Written.end(), expected.begin());
            if (std::memcmp(data + Offset(Levels + mip), expected.data(), MipBytes(mip))) return false;
        }
        return true;
    };
#ifdef BC4_FLOAT_MIP7_BUFFER_PROBE
    const auto poison_bridge = [&](const std::array<Word, 2>& expected) {
        const std::array<Word, 2> poison{~expected[0], ~expected[1]};
        std::memcpy(bridge_data + BridgeOffset, poison.data(), 8);
    };
    const auto buffer_copy = [&](bool forward) {
        const bool skip_source = is(forward ? "--skip-forward" : "--skip-reverse");
        const bool skip_upload = is(forward ? "--skip-forward-upload" : "--skip-reverse-upload");
        if (skip_source || skip_upload)
            std::printf("MUTATION %s after clean %s; omit %s leg only\n", mode + 2,
                        forward ? "setup" : "store", skip_source ? "source" : "destination");
        auto& src = forward ? parent : companion;
        auto& dst = forward ? companion : parent;
        runtime.Transit(&src, vk::ImageLayout::eTransferSrcOptimal,
                        vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferRead);
        runtime.Transit(&dst, vk::ImageLayout::eTransferDstOptimal,
                        vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferWrite);
        runtime.FlushBarriers();
        const vk::BufferImageCopy source{
            .bufferOffset = BridgeOffset, .bufferRowLength = 0, .bufferImageHeight = 0,
            .imageSubresource = {vk::ImageAspectFlagBits::eColor, forward ? TargetMip : 0, 0, 1},
            .imageOffset = {0, 0, 0}, .imageExtent = {forward ? 2u : 1u, 1, 1}};
        const vk::BufferImageCopy destination{
            .bufferOffset = BridgeOffset, .bufferRowLength = 0, .bufferImageHeight = 0,
            .imageSubresource = {vk::ImageAspectFlagBits::eColor, forward ? 0 : TargetMip, 0, 1},
            .imageOffset = {0, 0, 0}, .imageExtent = {forward ? 1u : 2u, 1, 1}};
        scheduler.Record([&, src_image = src.GetImage(), dst_image = dst.GetImage(), source,
                          destination, skip_source, skip_upload](vk::CommandBuffer cmd) {
            if (!skip_source)
                cmd.copyImageToBuffer(src_image, vk::ImageLayout::eTransferSrcOptimal, bridge, source, d);
            const vk::BufferMemoryBarrier2 barrier{
                .srcStageMask = vk::PipelineStageFlagBits2::eCopy,
                .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eCopy,
                .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .buffer = bridge, .offset = BridgeOffset, .size = 8};
            cmd.pipelineBarrier2({.bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &barrier}, d);
            if (!skip_upload)
                cmd.copyBufferToImage(bridge, dst_image, vk::ImageLayout::eTransferDstOptimal, destination, d);
        });
        if (forward) {
            forward_calls += !skip_source;
            forward_upload_calls += !skip_upload;
        } else {
            reverse_calls += !skip_source;
            reverse_upload_calls += !skip_upload;
        }
    };
#else
    const auto direct_copy = [&](bool forward) {
        auto& src = forward ? parent : companion;
        auto& dst = forward ? companion : parent;
        runtime.Transit(&src, vk::ImageLayout::eTransferSrcOptimal,
                        vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferRead);
        runtime.Transit(&dst, vk::ImageLayout::eTransferDstOptimal,
                        vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferWrite);
        runtime.FlushBarriers();
        const vk::ImageCopy region{
            .srcSubresource = {vk::ImageAspectFlagBits::eColor, forward ? TargetMip : 0, 0, 1},
            .dstSubresource = {vk::ImageAspectFlagBits::eColor, forward ? 0 : TargetMip, 0, 1},
            .extent = {forward ? 2u : 1u, 1, 1}};
        scheduler.Record([&, src_image = src.GetImage(), dst_image = dst.GetImage(), region](vk::CommandBuffer cmd) {
            cmd.copyImage(src_image, vk::ImageLayout::eTransferSrcOptimal, dst_image,
                          vk::ImageLayout::eTransferDstOptimal, region, d);
        });
        if (forward) ++forward_calls; else ++reverse_calls;
    };
#endif
    poison_parent(false); poison_float(Poison); flush_host();
    for (unsigned mip = 0; mip < Levels; ++mip) transfer(parent, mip, mip, true);
    transfer(companion, 0, FloatUpload, true);
    read_parent(); transfer(companion, 0, FloatRead, false); finish();
    Expect("clean setup: every parent mip equals distinct seed", parent_matches(false));
    Expect("clean setup: typed destination equals differing finite poison",
           !std::memcmp(data + Offset(FloatRead), Poison.data(), 8));
    Expect("clean setup: all readback boundaries intact", HostCanaries(data));
#ifdef BC4_FLOAT_MIP7_BUFFER_PROBE
    Expect("clean setup: dedicated bridge canaries intact", BridgeCanaries(bridge_data));
#endif
    if (failures) { std::puts("SETUP FAIL before mutation"); return shutdown(); }

#ifdef BC4_FLOAT_MIP7_BUFFER_PROBE
    poison_parent(false); poison_float(Initial); poison_bridge(Initial); flush_host();
    buffer_copy(true);
#else
    poison_parent(false); poison_float(Initial); flush_host();
    if (is("--skip-forward")) std::puts("MUTATION skip-forward after clean setup");
    else direct_copy(true);
#endif
    transfer(companion, 0, FloatRead, false); read_parent(); finish();
#ifdef BC4_FLOAT_MIP7_BUFFER_PROBE
    Expect("FORWARD BRIDGE oracle: source leg produced exact8 bytes",
           !std::memcmp(bridge_data + BridgeOffset, Initial.data(), 8));
    Expect("forward bridge canaries intact", BridgeCanaries(bridge_data));
    Expect("forward recorded leg counts match selected arm",
           forward_calls == unsigned(!is("--skip-forward")) &&
           forward_upload_calls == unsigned(!is("--skip-forward-upload")) &&
           reverse_calls == 0 && reverse_upload_calls == 0 && store_calls == 0);
#endif
    Expect("FORWARD oracle: exact8 bytes from compressed mip7", !std::memcmp(data + Offset(FloatRead), Initial.data(), 8));
    Expect("forward leaves all source parent mips unchanged", parent_matches(false));
    Expect("forward preserves host canaries", HostCanaries(data));
    if (failures) return shutdown();

    const vk::DescriptorSetLayoutBinding binding{.binding = 0, .descriptorType = vk::DescriptorType::eStorageImage,
        .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute};
    auto descriptors = Require(device.createDescriptorSetLayoutUnique({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = 1, .pBindings = &binding}, nullptr, d), "descriptor layout");
    auto layout = Require(device.createPipelineLayoutUnique({.setLayoutCount = 1,
        .pSetLayouts = &*descriptors}, nullptr, d), "pipeline layout");
    const auto code = FloatStoreShader();
    auto module = Require(device.createShaderModuleUnique({.codeSize = code.size() * sizeof(u32),
        .pCode = code.data()}, nullptr, d), "float shader");
    auto pipeline = Require(device.createComputePipelineUnique({}, {
        .stage = {.stage = vk::ShaderStageFlagBits::eCompute, .module = *module, .pName = "main"},
        .layout = *layout}, nullptr, d), "float pipeline");
    auto view = Require(device.createImageViewUnique({.image = companion.GetImage(),
        .viewType = vk::ImageViewType::e2D, .format = vk::Format::eR32G32Sfloat,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}}, nullptr, d), "float view");
    poison_float(Written); flush_host();
    runtime.Transit(&companion, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderStorageWrite);
    runtime.FlushBarriers();
    if (is("--skip-store")) std::puts("MUTATION skip-store after clean forward");
    else {
        scheduler.Record([&](vk::CommandBuffer cmd) {
            const vk::DescriptorImageInfo image{.imageView = *view, .imageLayout = vk::ImageLayout::eGeneral};
            const vk::WriteDescriptorSet write{.dstBinding = 0, .descriptorCount = 1,
                .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &image};
            cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline, d);
            cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *layout, 0, write, d);
            cmd.dispatch(1, 1, 1, d);
        });
        ++store_calls;
    }
    transfer(companion, 0, FloatRead, false); finish();
    Expect("STORE oracle: finite typedfloat output exact8 bytes", !std::memcmp(data + Offset(FloatRead), Written.data(), 8));
    Expect("storage readback preserves host canaries", HostCanaries(data));
#ifdef BC4_FLOAT_MIP7_BUFFER_PROBE
    Expect("storage leaves bridge bytes and canaries unchanged",
           !std::memcmp(bridge_data + BridgeOffset, Initial.data(), 8) && BridgeCanaries(bridge_data));
    Expect("storage recorded call count matches selected arm", store_calls == unsigned(!is("--skip-store")));
#endif
    if (failures) return shutdown();
#ifdef BC4_FLOAT_MIP7_BUFFER_PROBE
    poison_parent(true); poison_bridge(Written); flush_host();
    buffer_copy(false);
#else
    poison_parent(true); flush_host();
    if (is("--skip-reverse")) std::puts("MUTATION skip-reverse after clean store");
    else direct_copy(false);
#endif
    if (is("--skip-sibling-readback"))
        std::puts("MUTATION skip-sibling-readback after clean store; mip0 remains poisoned");
    read_parent(is("--skip-sibling-readback")); finish();
#ifdef BC4_FLOAT_MIP7_BUFFER_PROBE
    Expect("REVERSE BRIDGE oracle: source leg produced exact8 bytes",
           !std::memcmp(bridge_data + BridgeOffset, Written.data(), 8));
    Expect("reverse bridge canaries intact", BridgeCanaries(bridge_data));
    Expect("reverse recorded leg counts match selected arm",
           forward_calls == 1 && forward_upload_calls == 1 && store_calls == 1 &&
           reverse_calls == unsigned(!is("--skip-reverse")) &&
           reverse_upload_calls == unsigned(!is("--skip-reverse-upload")));
#endif
    Expect("REVERSE oracle: selected block equals typed store bytes",
           !std::memcmp(data + Offset(Levels + TargetMip), Written.data(), 8));
    bool siblings_match = true;
    for (unsigned mip = 0; mip < Levels; ++mip) {
        if (mip == TargetMip) continue;
        const auto expected = Seed(mip);
        if (std::memcmp(data + Offset(Levels + mip), expected.data(), MipBytes(mip)))
            siblings_match = false;
    }
    Expect("READBACK oracle: all eight sibling mips independently unchanged", siblings_match);
    Expect("reverse preserves host canaries", HostCanaries(data));
    if (failures) return shutdown();

    auto guest = GuestSeed();
    const auto* visible = data + Offset(Levels + TargetMip);
    if (is("--skip-publication")) std::puts("MUTATION skip-publication after clean reverse");
    else if (is("--overpublish")) {
        std::puts("MUTATION overpublish after clean reverse");
        std::memset(guest.data() + Guard + TailOffset, 0xcc, TailBytes);
        std::memcpy(guest.data() + Guard + TailOffset, visible, 8);
    } else std::memcpy(guest.data() + Guard + TailOffset, visible, 8);
    Expect("PUBLICATION oracle: only8 visible bytes changed;504padding/siblings/guards intact",
           PublicationMatches(guest));
    std::puts("Publication is a CPU byte-merge model; no TileImage/TextureCache/VM execution");
    return shutdown();
}
#endif
#else
// SPDX-License-Identifier: GPL-2.0-or-later
// Narrow device capability probe: actual Runtime::CopyImage in both directions, followed by
// a partial Rg32ui storage write. Optional BC4_TILE13_PROBE also checks guest retiling.
// Neither mode executes texture-cache replacement or VM publication; neither has a fallback.
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

namespace {
using Word = std::uint32_t;
#ifdef BC4_TILE13_PROBE
constexpr unsigned Grid = 16;
#else
constexpr unsigned Grid = 4;
#endif
constexpr unsigned Blocks = Grid * Grid, Bytes = Blocks * 8;
constexpr unsigned WriteX = 1, WriteY = 2, Changed = WriteY * Grid + WriteX;
constexpr Word WriteLo = 0xd4c3b2a1, WriteHi = 0x6987a5c3;
constexpr unsigned Guard = 32, Stride = Guard + Bytes + Guard, Slots = 5;
constexpr unsigned HostBytes = Stride * Slots;
using Payload = std::array<Word, Blocks * 2>;

constexpr unsigned Offset(unsigned slot) { return slot * Stride + Guard; }

Payload Seed() {
    Payload result{};
    for (unsigned b = 0; b < Blocks; ++b) {
        result[b * 2] = 0x10203040u + b * 0x01030507u;
        result[b * 2 + 1] = 0xfedcba98u - b * 0x07050301u;
    }
    return result;
}

Payload Patched() {
    auto result = Seed();
    result[Changed * 2] = WriteLo;
    result[Changed * 2 + 1] = WriteHi;
    return result;
}

bool Matches(const void* actual, const Payload& expected) {
    return std::memcmp(actual, expected.data(), Bytes) == 0;
}

bool Canaries(const unsigned char* data) {
    for (unsigned s = 0; s < Slots; ++s) {
        for (unsigned b = 0; b < Guard; ++b) {
            if (data[s * Stride + b] != 0xa5 || data[Offset(s) + Bytes + b] != 0xa5) {
                return false;
            }
        }
    }
    return true;
}

#ifdef BC4_TILE13_PROBE
Payload Tile13Oracle(const Payload& linear) {
    Payload tiled{};
    unsigned next = 0;
    // Enumerate each 8x8 microtile as recursive quadrants. This CPU traversal neither calls
    // the production shader nor consumes its offsets, and checks the complete tiled payload.
    const auto visit = [&](auto&& self, unsigned x, unsigned y, unsigned extent) -> void {
        if (extent == 1) {
            tiled[next++] = linear[2 * (y * Grid + x)];
            tiled[next++] = linear[2 * (y * Grid + x) + 1];
            return;
        }
        const unsigned half = extent / 2;
        self(self, x, y, half);
        self(self, x + half, y, half);
        self(self, x, y + half, half);
        self(self, x + half, y + half, half);
    };
    for (unsigned y = 0; y < Grid; y += 8)
        for (unsigned x = 0; x < Grid; x += 8) visit(visit, x, y, 8);
    if (next != tiled.size()) std::abort();
    return tiled;
}
#endif

bool OracleControls() {
    const auto seed = Seed(), patched = Patched();
#ifdef BC4_TILE13_PROBE
    const auto tiled = Tile13Oracle(patched);
    if (Matches(tiled.data(), patched)) return false;
    // Independent fixed coordinates: (1,2) occupies slot9 in the first Morton microtile.
    if (tiled[18] != WriteLo || tiled[19] != WriteHi) return false;
    for (unsigned word = 0; word < tiled.size(); ++word) {
        auto corrupt = tiled;
        corrupt[word] ^= 1;
        if (Matches(corrupt.data(), tiled)) return false;
    }
#endif
    if (!Matches(seed.data(), seed) || Matches(seed.data(), patched)) return false;
    for (unsigned b = 0; b < Blocks; ++b) {
        const bool same = std::memcmp(seed.data() + b * 2, patched.data() + b * 2, 8) == 0;
        if (same != (b != Changed)) return false;
        for (unsigned other = 0; other < b; ++other) {
            if (!std::memcmp(seed.data() + b * 2, seed.data() + other * 2, 8)) return false;
        }
    }
    for (unsigned w = 0; w < seed.size(); ++w) {
        auto corrupt = seed;
        corrupt[w] ^= 1;
        if (Matches(corrupt.data(), seed)) return false;
    }
    std::array<unsigned char, HostBytes> host;
    host.fill(0xa5);
    if (!Canaries(host.data())) return false;
    for (unsigned s = 0; s < Slots; ++s) {
        for (const unsigned at : {s * Stride, Offset(s) + Bytes}) {
            host[at] ^= 1;
            if (Canaries(host.data())) return false;
            host[at] ^= 1;
        }
    }
    return true;
}
} // namespace

#ifdef BC4_COPY_ORACLE_ONLY
int main() {
    const bool ok = OracleControls();
    std::printf("bc4-copy CPU oracle: %s (no Vulkan execution)\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
#else

#include <sirit/sirit.h>
#include "test_alarm.h"
#include "common/slot_vector.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/blit_helper.h"
#include "video_core/texture_cache/image.h"
#ifdef BC4_TILE13_PROBE
#include "video_core/texture_cache/tile_manager.h"
#endif
#include <vk_mem_alloc.h>

namespace {
unsigned failures = 0;
void Expect(const char* what, bool ok) {
    std::printf("%-72s %s\n", what, ok ? "ok" : "FAIL");
    failures += !ok;
}

template <typename T>
T Require(vk::ResultValue<T> result, const char* what) {
    if (result.result != vk::Result::eSuccess) {
        std::fprintf(stderr, "STOP: %s: Vulkan result %d; no fallback\n", what, int(result.result));
        std::exit(2);
    }
    return std::move(result.value);
}

VideoCore::ImageInfo Info(bool bc) {
    VideoCore::ImageInfo info{};
    info.type = AmdGpu::ImageType::Color2D;
    info.pixel_format = bc ? vk::Format::eBc4UnormBlock : vk::Format::eR32G32Uint;
    info.props.is_block = bc;
    info.size = {bc ? Grid * 4 : Grid, bc ? Grid * 4 : Grid, 1};
    info.pitch = info.size.width;
    info.num_bits = 64;
    info.resources = {1, 1};
    info.guest_size = Bytes;
#ifdef BC4_TILE13_PROBE
    info.props.is_tiled = true;
    info.tile_mode = AmdGpu::TileMode::Thin1DThin;
    info.array_mode = AmdGpu::ArrayMode::Array1DTiledThin1;
    info.mips_layout[0] = {Bytes, info.pitch, info.size.height, 0};
#endif
    return info;
}

std::vector<u32> StoreShader() {
    Sirit::Module m(0x00010500);
    m.AddCapability(spv::Capability::Shader);
    m.AddCapability(spv::Capability::StorageImageExtendedFormats);
    const auto u = m.TypeUInt(32), v2 = m.TypeVector(u, 2), v4 = m.TypeVector(u, 4);
    const auto image_type = m.TypeImage(u, spv::Dim::Dim2D, 0, false, false, 2,
                                      spv::ImageFormat::Rg32ui);
    const auto image = m.AddGlobalVariable(
        m.TypePointer(spv::StorageClass::UniformConstant, image_type),
        spv::StorageClass::UniformConstant);
    m.Decorate(image, spv::Decoration::DescriptorSet, 0u);
    m.Decorate(image, spv::Decoration::Binding, 0u);
    m.Decorate(image, spv::Decoration::NonReadable);
    const auto fn = m.OpFunction(m.TypeVoid(), spv::FunctionControlMask::MaskNone,
                                 m.TypeFunction(m.TypeVoid()));
    m.AddLabel(m.OpLabel());
    m.OpImageWrite(m.OpLoad(image_type, image),
                  m.ConstantComposite(v2, m.Constant(u, WriteX), m.Constant(u, WriteY)),
                  m.ConstantComposite(v4, m.Constant(u, WriteLo), m.Constant(u, WriteHi),
                                      m.Constant(u, 0u), m.Constant(u, 1u)));
    m.OpReturn();
    m.OpFunctionEnd();
    const std::array interfaces{image};
    m.AddEntryPoint(spv::ExecutionModel::GLCompute, fn, "main", interfaces);
    m.AddExecutionMode(fn, spv::ExecutionMode::LocalSize, 1u, 1u, 1u);
    m.SetMemoryModel(spv::AddressingModel::Logical, spv::MemoryModel::GLSL450);
    return m.Assemble();
}
} // namespace

int main(int argc, char** argv) {
    const char* mutation = argc == 2 ? argv[1] : "none";
    if (argc > 2 || (std::strcmp(mutation, "none") &&
                    std::strcmp(mutation, "--skip-forward") &&
                    std::strcmp(mutation, "--skip-reverse") &&
                    std::strcmp(mutation, "--skip-store")
#ifdef BC4_TILE13_PROBE
                    && std::strcmp(mutation, "--skip-retile")
#endif
                    )) {
        std::fputs("usage: bc4-integer-copy-test [--skip-forward|--skip-reverse|--skip-store]\n", stderr);
        return 2;
    }
    const bool forward = std::strcmp(mutation, "--skip-forward");
    const bool reverse = std::strcmp(mutation, "--skip-reverse");
    const bool store = std::strcmp(mutation, "--skip-store");
    // Fork the pause-aware watchdog before Vulkan creates threads. A timeout is nonzero.
    test_alarm(120);
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    Expect("CPU oracle and corruption controls", OracleControls());
    if (failures) return 1;
    Vulkan::Instance instance(0, false);
    vk::detail::DynamicLoader loader;
    vk::detail::DispatchLoaderDynamic d;
    d.init(loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr"));
    d.init(instance.GetInstance());
    d.init(instance.GetDevice());
    const auto device = instance.GetDevice();
    const auto properties = instance.GetPhysicalDevice().getProperties(d);
    std::printf("device=%s vendor=%#x driver=%#x mutation=%s\n", properties.deviceName.data(),
                properties.vendorID, properties.driverVersion, mutation);
    const auto transfers = vk::FormatFeatureFlagBits2::eTransferSrc |
                           vk::FormatFeatureFlagBits2::eTransferDst;
    if (!instance.IsFormatSupported(vk::Format::eBc4UnormBlock, transfers) ||
        !instance.IsFormatSupported(vk::Format::eR32G32Uint,
                                     transfers | vk::FormatFeatureFlagBits2::eStorageImage) ||
        !instance.GetPhysicalDevice().getFeatures(d).shaderStorageImageExtendedFormats) {
        std::fputs("STOP: BC4 transfer / RG32Uint transfer-storage capability unsupported; no fallback\n", stderr);
        return 2;
    }
    Vulkan::Scheduler scheduler(instance);
    Vulkan::Runtime runtime(instance, scheduler);
    scheduler.SetSubmitCallback([&](Vulkan::SubmitInfo&) { runtime.FlushBarriers(); });
    Common::SlotVector<VideoCore::ImageView> views;
    VideoCore::Image source(instance, runtime, views, Info(true));
    VideoCore::Image integer(instance, runtime, views, Info(false));
    VideoCore::Image destination(instance, runtime, views, Info(true));
    Expect("actual backings retain BC4 / RG32Uint / BC4 formats",
           source.backing->image.image_ci.format == vk::Format::eBc4UnormBlock &&
           integer.backing->image.image_ci.format == vk::Format::eR32G32Uint &&
           destination.backing->image.image_ci.format == vk::Format::eBc4UnormBlock);
    Expect("integer backing has real storage usage",
           bool(integer.usage_flags & vk::ImageUsageFlagBits::eStorage));
    if (failures) { scheduler.Close(); return 1; }

    VkBuffer host{};
    VmaAllocation allocation{};
    VmaAllocationInfo mapped{};
    const VkBufferCreateInfo bi{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                               .size = HostBytes,
                               .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    const VmaAllocationCreateInfo ac{.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                                             VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
                                    .usage = VMA_MEMORY_USAGE_AUTO,
                                    .requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT};
    const auto allocated = vmaCreateBuffer(instance.GetAllocator(), &bi, &ac, &host, &allocation, &mapped);
    if (allocated != VK_SUCCESS) {
        std::fprintf(stderr, "STOP: host allocation failed: %d\n", allocated);
        return 2;
    }
    auto* data = static_cast<unsigned char*>(mapped.pMappedData);
    std::memset(data, 0xa5, HostBytes);
    const auto seed = Seed(), patched = Patched();
    auto poison = seed;
    for (auto& word : poison) word ^= 0xffffffffu;
    std::memcpy(data + Offset(0), seed.data(), Bytes);
    std::memcpy(data + Offset(1), poison.data(), Bytes);

    const auto transfer = [&](VideoCore::Image& image, unsigned slot, bool upload) {
        const auto layout = upload ? vk::ImageLayout::eTransferDstOptimal : vk::ImageLayout::eTransferSrcOptimal;
        runtime.Transit(&image, layout, vk::PipelineStageFlagBits2::eCopy,
                        upload ? vk::AccessFlagBits2::eTransferWrite : vk::AccessFlagBits2::eTransferRead);
        runtime.FlushBarriers();
        const vk::BufferImageCopy region{
            .bufferOffset = Offset(slot),
            .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .imageExtent = {image.info.size.width, image.info.size.height, 1}};
        scheduler.Record([&, handle = image.GetImage(), layout, region, upload](vk::CommandBuffer cmd) {
            if (upload) cmd.copyBufferToImage(host, handle, layout, region, d);
            else cmd.copyImageToBuffer(handle, layout, host, region, d);
        });
    };
    const auto finish_readback = [&] {
        scheduler.Record([&](vk::CommandBuffer cmd) {
            const vk::MemoryBarrier2 barrier{.srcStageMask = vk::PipelineStageFlagBits2::eCopy,
                                              .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                                              .dstStageMask = vk::PipelineStageFlagBits2::eHost,
                                              .dstAccessMask = vk::AccessFlagBits2::eHostRead};
            cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &barrier}, d);
        });
        scheduler.Finish();
    };
    transfer(source, 0, true);
    transfer(integer, 1, true);
    transfer(destination, 1, true);
    if (forward) runtime.CopyImage(&source, &integer);
    transfer(integer, 2, false);
    if (reverse) runtime.CopyImage(&integer, &destination);
    transfer(destination, 3, false);
    finish_readback();
    Expect("BC4 -> uint: all integer texels match original raw blocks", Matches(data + Offset(2), seed));
    Expect("uint -> BC4: all roundtrip blocks match original bytes", Matches(data + Offset(3), seed));
    Expect("first roundtrip: every host boundary canary intact", Canaries(data));

    const vk::DescriptorSetLayoutBinding binding{.binding = 0,
        .descriptorType = vk::DescriptorType::eStorageImage, .descriptorCount = 1,
        .stageFlags = vk::ShaderStageFlagBits::eCompute};
    auto descriptors = Require(device.createDescriptorSetLayoutUnique({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = 1, .pBindings = &binding}, nullptr, d), "storage descriptor layout");
    auto layout = Require(device.createPipelineLayoutUnique({
        .setLayoutCount = 1, .pSetLayouts = &*descriptors}, nullptr, d), "pipeline layout");
    const auto code = StoreShader();
    auto module = Require(device.createShaderModuleUnique({
        .codeSize = code.size() * sizeof(u32), .pCode = code.data()}, nullptr, d), "storage shader");
    auto pipeline = Require(device.createComputePipelineUnique({}, {
        .stage = {.stage = vk::ShaderStageFlagBits::eCompute, .module = *module, .pName = "main"},
        .layout = *layout}, nullptr, d), "storage pipeline");
    auto view = Require(device.createImageViewUnique({.image = integer.GetImage(),
        .viewType = vk::ImageViewType::e2D, .format = vk::Format::eR32G32Uint,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}}, nullptr, d), "integer storage view");
    runtime.Transit(&integer, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderStorageWrite);
    runtime.FlushBarriers();
    if (store) {
        scheduler.Record([&](vk::CommandBuffer cmd) {
            const vk::DescriptorImageInfo image{.imageView = *view, .imageLayout = vk::ImageLayout::eGeneral};
            const vk::WriteDescriptorSet write{.dstBinding = 0, .descriptorCount = 1,
                .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &image};
            cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline, d);
            cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *layout, 0, write, d);
            cmd.dispatch(1, 1, 1, d);
        });
    }
    if (reverse) runtime.CopyImage(&integer, &destination);
    transfer(integer, 2, false);
    transfer(destination, 3, false);
    transfer(source, 4, false);
    finish_readback();
    Expect("storage dispatch: uint payload equals CPU partial-write oracle", Matches(data + Offset(2), patched));
    Expect("copied BC4: changed block exact and all others untouched", Matches(data + Offset(3), patched));
    Expect("original BC4 source remains byte-identical", Matches(data + Offset(4), seed));
    Expect("partial write: every host boundary canary intact", Canaries(data));
    std::printf("path calls: Runtime::CopyImage BC4->uint=%u uint->BC4=%u; storage dispatch=%u; "
                "completed readback phases=2; mip=0 layer=0\n", unsigned(forward), 2u * unsigned(reverse), unsigned(store));
#ifdef BC4_TILE13_PROBE
    {
        VideoCore::StreamBuffer params(instance, scheduler, VideoCore::MemoryType::Stream, 65536);
        VideoCore::TileManager tiler(instance, scheduler, runtime, params);
        const u64 prefix = std::max<u64>(256, instance.StorageMinAlignment());
        const u64 stride = ((Bytes + prefix - 1) / prefix + 1) * prefix;
        const u64 length = prefix + 2 * stride;
        VideoCore::Buffer output(instance, 0, length, VideoCore::MemoryType::HostCached);
        std::memset(output.mapped_data.data(), 0xa5, length);
        output.Flush(0, length);
        const bool retile = std::strcmp(mutation, "--skip-retile");
        unsigned tile_calls = 0, raw_calls = 0;
        const std::array<VideoCore::Image*, 2> images{&integer, &destination};
        for (unsigned index = 0; index < images.size(); ++index) {
            auto& image = *images[index];
            Expect("TileImage input is exact64-bit single-level tile13 representation",
                   image.info.num_bits == 64 && image.info.props.is_tiled &&
                   image.info.tile_mode == AmdGpu::TileMode::Thin1DThin &&
                   image.info.array_mode == AmdGpu::ArrayMode::Array1DTiledThin1 &&
                   image.info.resources.levels == 1 && image.info.resources.layers == 1 &&
                   image.info.mips_layout[0].size == Bytes &&
                   image.info.mips_layout[0].pitch == image.info.size.width &&
                   image.info.mips_layout[0].height == image.info.size.height &&
                   image.info.props.is_block == (index == 1));
            vk::BufferImageCopy region{
                .bufferRowLength = image.info.pitch,
                .bufferImageHeight = image.info.size.height,
                .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                .imageExtent = {image.info.size.width, image.info.size.height, 1}};
            const u64 offset = prefix + index * stride;
            if (retile) {
                tiler.TileImage(image, std::span{&region, 1}, &output, offset);
                ++tile_calls;
            } else {
                region.bufferOffset = offset;
                runtime.DownloadImage(&image, &output, std::span{&region, 1});
                ++raw_calls;
            }
        }
        runtime.FlushBarriers();
        scheduler.Record([&](vk::CommandBuffer cmd) {
            const vk::MemoryBarrier2 barrier{
                .srcStageMask = vk::PipelineStageFlagBits2::eCopy |
                                vk::PipelineStageFlagBits2::eComputeShader,
                .srcAccessMask = vk::AccessFlagBits2::eTransferWrite |
                                 vk::AccessFlagBits2::eShaderWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eHost,
                .dstAccessMask = vk::AccessFlagBits2::eHostRead};
            cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &barrier}, d);
        });
        scheduler.Finish();
        output.Invalidate(0, length);
        const auto expected = Tile13Oracle(patched);
        Expect("integer TileImage: all256 blocks equal independent CPU tile13 oracle",
               Matches(output.mapped_data.data() + prefix, expected));
        Expect("BC4 TileImage: compressed pitch normalized, all256 blocks exact",
               Matches(output.mapped_data.data() + prefix + stride, expected));
        bool guards = true;
        for (u64 byte = 0; byte < length; ++byte) {
            const bool payload = (byte >= prefix && byte < prefix + Bytes) ||
                                 (byte >= prefix + stride && byte < prefix + stride + Bytes);
            if (!payload && output.mapped_data[byte] != 0xa5) guards = false;
        }
        Expect("both tiled readback payloads preserve every surrounding canary", guards);
        std::printf("TileImage actualcalls=%u, raw-download mutation=%u, mip0 layer0; "
                    "no TextureCache/VM publication execution\n", tile_calls, raw_calls);
    }
#endif
    scheduler.Close();
    vmaDestroyBuffer(instance.GetAllocator(), host, allocation);
    test_alarm(0);
    std::printf("bc4-integer-copy-test: %s (%u failures); primitive only, no cache repair proof\n",
                failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
#endif

#endif // BC4_FLOAT_MIP7_PROBE
