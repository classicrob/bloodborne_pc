// SPDX-License-Identifier: GPL-2.0-or-later
// Narrow device capability probe: actual Runtime::CopyImage in both directions, followed by
// a partial Rg32ui storage write. No texture-cache replacement, guest tiling or fallback.
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

namespace {
using Word = std::uint32_t;
constexpr unsigned Grid = 4, Blocks = Grid * Grid, Bytes = Blocks * 8;
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

bool OracleControls() {
    const auto seed = Seed(), patched = Patched();
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
                    std::strcmp(mutation, "--skip-store"))) {
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
    Expect("BC4 -> uint: all 16 integer texels match original raw blocks", Matches(data + Offset(2), seed));
    Expect("uint -> BC4: all 16 roundtrip blocks match original bytes", Matches(data + Offset(3), seed));
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
    Expect("copied BC4: changed block exact and all 15 others untouched", Matches(data + Offset(3), patched));
    Expect("original BC4 source remains byte-identical", Matches(data + Offset(4), seed));
    Expect("partial write: every host boundary canary intact", Canaries(data));
    std::printf("path calls: Runtime::CopyImage BC4->uint=%u uint->BC4=%u; storage dispatch=%u; "
                "completed readback phases=2; mip=0 layer=0\n", unsigned(forward), 2u * unsigned(reverse), unsigned(store));
    scheduler.Close();
    vmaDestroyBuffer(instance.GetAllocator(), host, allocation);
    test_alarm(0);
    std::printf("bc4-integer-copy-test: %s (%u failures); primitive only, no cache repair proof\n",
                failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
#endif
