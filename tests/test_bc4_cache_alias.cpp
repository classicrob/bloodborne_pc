// SPDX-License-Identifier: GPL-2.0-or-later
// Actual TextureCache and Mach backing publication, using the production VM hooks.
// Direct Stage-A cache operations do not exercise guest DrawRecord packet admission.
#include <algorithm>
#include <array>
#include <vector>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <signal.h>
#include <unistd.h>
#include <sirit/sirit.h>
#include "../gpu/bbgpu.h"
#include "common/elf_info.h"
#ifdef BC4_CACHE_FLOAT_TAIL_PROBE
#include "core/emulator_settings.h"
#endif
#include "test_alarm.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/bc4_alias.h"
#include "video_core/texture_cache/texture_cache.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/buffer_cache.h"
extern "C" {
#include "../src/runtime.h"
#include "../src/runtime_memory.h"
int bbgpu_handle_fault(void*, void*);
void runtime_memory_set_gpu_hooks(void (*)(uintptr_t, uint64_t),
                                 void (*)(uintptr_t, uint64_t),
                                 void (*)(uintptr_t, uint64_t));
// Only non-memory host services are stubbed. The target links runtime_memory.c itself.
uintptr_t runtime_lookup(const RuntimeExport* table, size_t n, const char* name) {
    for (size_t i = 0; i < n; ++i)
        if (!std::strcmp(table[i].name, name)) return uintptr_t(table[i].function);
    return 0;
}
uint64_t runtime_process_time_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
uint64_t runtime_process_time_counter() { return runtime_process_time_us() * 1000; }
uint64_t runtime_tsc_frequency() { return 1000000000; }
int runtime_file_translate(const char*, char*, size_t) { std::abort(); }
void runtime_hle_trace_dump() { std::abort(); }
void runtime_hle_trace_frame() {}
void runtime_cpu_profile_thread(const char*) {}
void runtime_thread_attach_host(const char*) {}
void runtime_thread_run_host(void (*)(void*), void*) { std::abort(); }
void runtime_restart() { std::abort(); }
int32_t* runtime_errno() { std::abort(); }
}

// Keep this friend-class definition identical to gpu/shim/bbgpu.cpp. Only the
// normal metadata initialization is needed; bbgpu_init also creates a presenter.
namespace Core {
class Emulator {
public:
    static void FillElfInfo(const BbGpuConfig& config) {
        auto& info = Common::ElfInfo::Instance();
        info.initialized = true;
        info.game_serial = config.serial ? config.serial : "UNKNOWN";
        info.title = config.title ? config.title : "";
        info.sdk_ver = config.sdk_version;
        info.psf_attributes.raw = config.psf_attributes;
    }
};
} // namespace Core

namespace {
constexpr unsigned Page = 16384;
#ifndef BC4_CACHE_FLOAT_TAIL_PROBE
constexpr unsigned Grid = 16, Bytes = Grid * Grid * 8, Offset = 256;
constexpr unsigned WriteX = 1, WriteY = 2;
constexpr u32 WriteLo = 0xd4c3b2a1, WriteHi = 0x6987a5c3;
using Payload = std::array<u32, Grid * Grid * 2>;
#endif
using Cache = VideoCore::TextureCache;
unsigned failures = 0;
volatile sig_atomic_t write_faults = 0;

void Expect(const char* label, bool ok) {
    std::printf("%s: %s\n", label, ok ? "ok" : "FAIL");
    failures += !ok;
}
void RequireNegativeSetup() {
    if (!failures) return;
    std::fputs("SETUP FAIL: earlier oracle failed; negative target not reached\n", stderr);
    _exit(2);
}
void Fault(int signal, siginfo_t* info, void* context) {
    if (bbgpu_handle_fault(context, info->si_addr)) {
        write_faults = write_faults + 1;
        return;
    }
    _exit(128 + signal);
}
#ifndef BC4_CACHE_FLOAT_TAIL_PROBE
Payload Seed(unsigned generation, bool written = false) {
    Payload p{};
    for (unsigned i = 0; i < p.size(); ++i)
        p[i] = (0x12345678u + i * 0x01030507u) ^ (generation * 0x9e3779b9u);
    if (written) {
        p[2 * (WriteY * Grid + WriteX)] = WriteLo;
        p[2 * (WriteY * Grid + WriteX) + 1] = WriteHi;
    }
    return p;
}
Payload Tiled(const Payload& linear) {
    Payload result{};
    unsigned next = 0;
    const auto visit = [&](auto&& self, unsigned x, unsigned y, unsigned extent) -> void {
        if (extent == 1) {
            result[next++] = linear[2 * (y * Grid + x)];
            result[next++] = linear[2 * (y * Grid + x) + 1];
        } else {
            const unsigned half = extent / 2;
            self(self, x, y, half); self(self, x + half, y, half);
            self(self, x, y + half, half); self(self, x + half, y + half, half);
        }
    };
    for (unsigned y = 0; y < Grid; y += 8)
        for (unsigned x = 0; x < Grid; x += 8) visit(visit, x, y, 8);
    assert(next == result.size());
    return result;
}
bool Matches(const void* bytes, const Payload& expected) {
    return !std::memcmp(bytes, expected.data(), Bytes);
}
bool Canaries(const unsigned char* page) {
    for (unsigned i = 0; i < Page; ++i)
        if ((i < Offset || i >= Offset + Bytes) && page[i] != 0xa5) return false;
    return true;
}
Cache::ImageDesc Desc(uintptr_t address, bool bc) {
    AmdGpu::Image sharp = AmdGpu::Image::Null(false);
    sharp.base_address = address >> 8;
    sharp.data_format = u64(bc ? AmdGpu::DataFormat::FormatBc4 : AmdGpu::DataFormat::Format32_32);
    sharp.num_format = u64(bc ? AmdGpu::NumberFormat::Unorm : AmdGpu::NumberFormat::Uint);
    sharp.width = sharp.height = sharp.pitch = (bc ? Grid * 4 : Grid) - 1;
    Shader::ImageResource resource{};
    resource.is_written = !bc;
    return {sharp, resource};
}
#endif
template <class T> T Require(vk::ResultValue<T> result) {
    assert(result.result == vk::Result::eSuccess);
    return std::move(result.value);
}

#ifndef BC4_CACHE_FLOAT_TAIL_PROBE
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
#endif
} // namespace

#ifdef BC4_CACHE_FLOAT_TAIL_PROBE
namespace FloatCache {
constexpr unsigned Levels = 9, TargetMip = 7, GuestSize = 0x6000, Tail = 0x5c00;
constexpr unsigned MapSize = 0x8000, Prefix = 256, ReadGuard = 32;
constexpr std::array<unsigned, Levels> MipOffsets{0, 0x4000, 0x5000, 0x5400, 0x5600,
                                                0x5800, 0x5a00, 0x5c00, 0x5e00};
constexpr std::array<unsigned, Levels> MipSizes{0x4000, 0x1000, 0x400, 0x200, 0x200,
                                              0x200, 0x200, 0x200, 0x200};
using Guest = std::array<u8, MapSize>;
constexpr std::array<u32, 2> Written{0x3f000000, 0x40400000};
unsigned Width(unsigned mip) { return std::max(256u >> mip, 1u); }
unsigned Height(unsigned mip) { return std::max(128u >> mip, 1u); }
unsigned BlocksX(unsigned mip) { return (Width(mip) + 3) / 4; }
unsigned BlocksY(unsigned mip) { return (Height(mip) + 3) / 4; }
unsigned TiledBlock(unsigned mip, unsigned x, unsigned y) {
    unsigned morton = 0;
    for (unsigned bit = 0; bit < 3; ++bit) {
        morton |= ((x >> bit) & 1) << (2 * bit);
        morton |= ((y >> bit) & 1) << (2 * bit + 1);
    }
    const unsigned tiles_x = (BlocksX(mip) + 7) / 8;
    return ((y / 8) * tiles_x + x / 8) * 64 + morton;
}
std::vector<u32> Linear(unsigned generation, unsigned mip, bool written = false) {
    std::vector<u32> result(BlocksX(mip) * BlocksY(mip) * 2);
    for (unsigned i = 0; i < result.size(); ++i)
        result[i] = (i & 1 ? 0x40000000u : 0x3f000000u) +
                    generation * 0x10000u + mip * 0x800u + i;
    if (written && mip == TargetMip) std::copy(Written.begin(), Written.end(), result.begin());
    return result;
}
Guest Seed(unsigned generation, bool written = false) {
    Guest result;
    for (unsigned i = 0; i < result.size(); ++i) result[i] = u8(11 + i * 37);
    for (unsigned mip = 0; mip < Levels; ++mip) {
        const auto linear = Linear(generation, mip, written);
        for (unsigned y = 0; y < BlocksY(mip); ++y)
            for (unsigned x = 0; x < BlocksX(mip); ++x)
                std::memcpy(result.data() + Prefix + MipOffsets[mip] + TiledBlock(mip, x, y) * 8,
                            linear.data() + 2 * (y * BlocksX(mip) + x), 8);
    }
    return result;
}
bool OracleControls() {
    if (TiledBlock(0, 1, 2) != 9 || MipOffsets.back() + MipSizes.back() != GuestSize)
        return false;
    const auto before = Seed(1), after = Seed(1, true);
    auto expected = before;
    std::memcpy(expected.data() + Prefix + Tail, Written.data(), 8);
    if (expected != after || before == after) return false;
    for (unsigned i = 0; i < MapSize; ++i) {
        expected[i] ^= 1;
        if (expected == after) return false;
        expected[i] ^= 1;
    }
    for (unsigned mip = 0; mip < Levels; ++mip) {
        std::vector<bool> occupied(MipSizes[mip] / 8);
        for (unsigned y = 0; y < BlocksY(mip); ++y)
            for (unsigned x = 0; x < BlocksX(mip); ++x) {
                const auto index = TiledBlock(mip, x, y);
                if (index >= occupied.size() || occupied[index]) return false;
                occupied[index] = true;
            }
    }
    return true;
}
Cache::ImageDesc Desc(uintptr_t base, bool parent) {
    auto sharp = AmdGpu::Image::Null(false);
    sharp.base_address = (base + (parent ? 0 : Tail)) >> 8;
    sharp.data_format = u64(parent ? AmdGpu::DataFormat::FormatBc4 : AmdGpu::DataFormat::Format32_32);
    sharp.num_format = u64(parent ? AmdGpu::NumberFormat::Unorm : AmdGpu::NumberFormat::Float);
    sharp.width = parent ? 255 : 0;
    sharp.height = parent ? 127 : 0;
    sharp.pitch = parent ? 255 : 7;
    sharp.last_level = parent ? 8 : 0;
    sharp.tiling_index = 13;
    sharp.dst_sel_x = 4; sharp.dst_sel_y = 5; sharp.dst_sel_z = 0; sharp.dst_sel_w = 1;
    Shader::ImageResource resource{};
    resource.is_written = !parent;
    resource.is_array = true;
    return {sharp, resource};
}
std::vector<u32> StoreShader() {
    Sirit::Module m(0x00010500);
    m.AddCapability(spv::Capability::Shader);
    m.AddCapability(spv::Capability::StorageImageExtendedFormats);
    const auto u = m.TypeUInt(32), f = m.TypeFloat(32), b = m.TypeBool();
    const auto v2 = m.TypeVector(u, 2), v4 = m.TypeVector(f, 4);
    const auto image_type = m.TypeImage(f, spv::Dim::Dim2D, 0, false, false, 2, spv::ImageFormat::Rg32f);
    const auto image = m.AddGlobalVariable(m.TypePointer(spv::StorageClass::UniformConstant, image_type),
                                          spv::StorageClass::UniformConstant);
    m.Decorate(image, spv::Decoration::DescriptorSet, 0u);
    m.Decorate(image, spv::Decoration::Binding, 0u);
    m.Decorate(image, spv::Decoration::NonReadable);
    const auto push_type = m.TypeStruct(u);
    m.Decorate(push_type, spv::Decoration::Block);
    m.MemberDecorate(push_type, 0, spv::Decoration::Offset, 0u);
    const auto push = m.AddGlobalVariable(m.TypePointer(spv::StorageClass::PushConstant, push_type),
                                         spv::StorageClass::PushConstant);
    const auto fn = m.OpFunction(m.TypeVoid(), spv::FunctionControlMask::MaskNone,
                                 m.TypeFunction(m.TypeVoid()));
    m.AddLabel(m.OpLabel());
    const auto enabled = m.OpLoad(u, m.OpAccessChain(m.TypePointer(spv::StorageClass::PushConstant, u),
                                                   push, m.Constant(u, 0u)));
    const auto store = m.OpLabel(), merge = m.OpLabel();
    const auto condition = m.OpINotEqual(b, enabled, m.Constant(u, 0u));
    m.OpSelectionMerge(merge, spv::SelectionControlMask::MaskNone);
    m.OpBranchConditional(condition, store, merge);
    m.AddLabel(store);
    m.OpImageWrite(m.OpLoad(image_type, image),
                  m.ConstantComposite(v2, m.Constant(u, 0u), m.Constant(u, 0u)),
                  m.ConstantComposite(v4, m.Constant(f, 0.5f), m.Constant(f, 3.0f),
                                      m.Constant(f, 0.0f), m.Constant(f, 1.0f)));
    m.OpBranch(merge);
    m.AddLabel(merge);
    m.OpReturn(); m.OpFunctionEnd();
    const std::array interfaces{image, push};
    m.AddEntryPoint(spv::ExecutionModel::GLCompute, fn, "main", interfaces);
    m.AddExecutionMode(fn, spv::ExecutionMode::LocalSize, 1u, 1u, 1u);
    m.SetMemoryModel(spv::AddressingModel::Logical, spv::MemoryModel::GLSL450);
    return m.Assemble();
}

template <class Shutdown>
int Run(Vulkan::Instance& instance, Vulkan::Scheduler& scheduler, Vulkan::Runtime& runtime,
        AmdGpu::Liverpool& liverpool, Vulkan::Rasterizer& rasterizer,
        vk::detail::DispatchLoaderDynamic& d, void* mapped, int64_t physical,
        const char* mode, Shutdown&& shutdown) {
    auto& cache = rasterizer.GetTextureCache();
    const auto device = instance.GetDevice();
    auto* guest = static_cast<u8*>(mapped);
    const auto base = uintptr_t(guest) + Prefix;
    const auto is = [&](const char* value) { return !std::strcmp(mode, value); };
    const auto on_a = [&](auto&& call) { liverpool.SendCommand<true>(std::forward<decltype(call)>(call)); };
    using Allocate = int32_t (ABI *)(int64_t, int64_t, uint64_t, uint64_t, int, int64_t*);
    using Map = int32_t (ABI *)(void**, uint64_t, int, int, int64_t, uint64_t);
    using Release = int32_t (ABI *)(int64_t, uint64_t);
    const auto allocate = reinterpret_cast<Allocate>(runtime_memory_resolve("sceKernelAllocateDirectMemory"));
    const auto map = reinterpret_cast<Map>(runtime_memory_resolve("sceKernelMapDirectMemory"));
    const auto release = reinterpret_cast<Release>(runtime_memory_resolve("sceKernelReleaseDirectMemory"));
    const auto complete = [&] {
        assert(!release(physical, MapSize));
        shutdown();
        std::printf("bc4-float-cache-alias-test: %s (%u failures); direct cache/completed GPU/VM only\n",
                    failures ? "FAIL" : "PASS", failures);
        return failures ? 1 : 0;
    };
    Expect("Float full-allocation byte/morton oracle controls", OracleControls());
    Expect("generic linear readback is explicitly enabled", EmulatorSettings.IsReadbackLinearImagesEnabled());
    auto parent_desc = Desc(base, true), child_desc = Desc(base, false);
    Expect("exact Float-tail geometry admitted by production helper",
           !VideoCore::Bc4Alias::RejectFloatTail(child_desc.info, parent_desc.info, TargetMip));
    bool layout_ok = parent_desc.info.guest_size == GuestSize;
    for (unsigned mip = 0; mip < Levels; ++mip) {
        const auto& layout = parent_desc.info.mips_layout[mip];
        layout_ok &= layout.offset == MipOffsets[mip] && layout.size == MipSizes[mip] &&
                     layout.pitch == std::max(Width(mip), 32u) &&
                     layout.height == std::max(Height(mip), 32u);
    }
    Expect("all nine guest mip layouts match independent fixed layout", layout_ok);
    if (failures) { std::puts("SETUP FAIL before Float selection"); return complete(); }
    const auto seed0 = Seed(0);
    std::memcpy(guest, seed0.data(), MapSize);
    VideoCore::Buffer readback(instance, 0, MapSize, VideoCore::MemoryType::HostCached);
    const auto host_finish = [&] {
        runtime.FlushBarriers();
        scheduler.Record([&](vk::CommandBuffer cmd) {
            const vk::MemoryBarrier2 barrier{.srcStageMask = vk::PipelineStageFlagBits2::eCopy,
                .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eHost,
                .dstAccessMask = vk::AccessFlagBits2::eHostRead};
            cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &barrier}, d);
        });
        scheduler.Finish();
        readback.Invalidate(0, MapSize);
    };
    const auto read_image = [&](VideoCore::ImageId id, unsigned mip, const std::vector<u32>& expected,
                                const char* label) {
        std::memset(readback.mapped_data.data(), 0xa5, MapSize);
        auto poison = expected;
        for (auto& word : poison) word = ~word;
        std::memcpy(readback.mapped_data.data() + ReadGuard, poison.data(), poison.size() * 4);
        readback.Flush(0, MapSize);
        auto& image = cache.GetImage(id);
        const vk::BufferImageCopy region{.bufferOffset = ReadGuard,
            .imageSubresource = {vk::ImageAspectFlagBits::eColor, mip, 0, 1},
            .imageExtent = {std::max(image.info.size.width >> mip, 1u),
                            std::max(image.info.size.height >> mip, 1u), 1}};
        runtime.DownloadImage(&image, &readback, std::span{&region, 1});
        host_finish();
        Expect(label, !std::memcmp(readback.mapped_data.data() + ReadGuard, expected.data(), expected.size() * 4));
        bool guards = true;
        for (unsigned i = 0; i < MapSize; ++i)
            if ((i < ReadGuard || i >= ReadGuard + expected.size() * 4) && readback.mapped_data[i] != 0xa5)
                guards = false;
        Expect("Float GPU readback surrounding canaries", guards);
    };
    VideoCore::ImageId parent_id{}, child_id{};
    u64 parent_uid{}, child_uid{};
    Cache::ViewMemo memo{};
    const auto select_parent = [&] {
        parent_id = cache.FindImage(parent_desc);
        cache.FindTexture(parent_id, parent_desc);
        parent_uid = cache.GetImage(parent_id).image_uid;
    };
    const auto read_parent = [&](unsigned generation, bool written) {
        for (unsigned mip = 0; mip < Levels; ++mip)
            read_image(parent_id, mip, Linear(generation, mip, written), "Float parent actual mip payload");
    };
    const vk::DescriptorSetLayoutBinding binding{.binding = 0,
        .descriptorType = vk::DescriptorType::eStorageImage, .descriptorCount = 1,
        .stageFlags = vk::ShaderStageFlagBits::eCompute};
    auto descriptors = Require(device.createDescriptorSetLayoutUnique({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = 1, .pBindings = &binding}, nullptr, d));
    const vk::PushConstantRange push{.stageFlags = vk::ShaderStageFlagBits::eCompute, .offset = 0, .size = 4};
    auto layout = Require(device.createPipelineLayoutUnique({.setLayoutCount = 1,
        .pSetLayouts = &*descriptors, .pushConstantRangeCount = 1, .pPushConstantRanges = &push}, nullptr, d));
    const auto code = StoreShader();
    auto module = Require(device.createShaderModuleUnique({.codeSize = code.size() * sizeof(u32),
        .pCode = code.data()}, nullptr, d));
    auto pipeline = Require(device.createComputePipelineUnique({}, {
        .stage = {.stage = vk::ShaderStageFlagBits::eCompute, .module = *module, .pName = "main"},
        .layout = *layout}, nullptr, d));
    unsigned no_store_dispatches = 0, store_dispatches = 0;
    const auto command = [&](unsigned generation, bool write, bool publish, bool skip_dispatch = false) {
        {
            const std::array selected{child_desc};
            auto snapshot = cache.InspectBc4FloatParents(selected);
            Expect("Float preflight authenticates existing parent identity",
                   snapshot.parents.size() == 1 && snapshot.parents[0].parent_id == parent_id &&
                   snapshot.parents[0].parent_uid == parent_uid && snapshot.parents[0].mip == TargetMip);
            RequireNegativeSetup();
            auto scope = cache.UseBc4FloatPreflight(snapshot);
            child_id = cache.FindImage(child_desc);
            auto& view = cache.FindTexture(child_id, child_desc, &memo);
            child_uid = cache.GetImage(child_id).image_uid;
            Expect("Float selection returns separate typed companion",
                   child_id != parent_id && view.info.format == vk::Format::eR32G32Sfloat && view.info.is_storage);
            read_image(child_id, 0, Linear(generation, TargetMip), "Float companion initialized from current parent8B");
            RequireNegativeSetup();
            runtime.Transit(&cache.GetImage(child_id), vk::ImageLayout::eGeneral,
                            vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderStorageWrite);
            runtime.FlushBarriers();
            if (skip_dispatch) std::puts("MUTATION Float skip-store after clean initialization");
            else {
                const u32 enabled = write;
                scheduler.Record([&, handle = *view.image_view, enabled](vk::CommandBuffer cmd) {
                    const vk::DescriptorImageInfo info{.imageView = handle, .imageLayout = vk::ImageLayout::eGeneral};
                    const vk::WriteDescriptorSet descriptor{.dstBinding = 0, .descriptorCount = 1,
                        .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &info};
                    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline, d);
                    cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *layout, 0, descriptor, d);
                    cmd.pushConstants(*layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(enabled), &enabled, d);
                    cmd.dispatch(1, 1, 1, d);
                });
                if (write) ++store_dispatches; else ++no_store_dispatches;
            }
            scheduler.Finish();
        }
        if (publish) {
            cache.MaterializeBc4AliasesAfterCommand();
            cache.ProcessDownloadImages();
            scheduler.Finish();
        }
    };
    on_a([&] { select_parent(); read_parent(0, false); });
    if (failures) { std::puts("SETUP FAIL before Float mutations"); return complete(); }
    const auto seed1 = Seed(1);
    const auto faults = write_faults;
    std::memcpy(guest, seed1.data(), MapSize);
    Expect("Float CPU rewrite traversed tracked fault", write_faults > faults);
    on_a([&] {
        Expect("Float parent CPU-dirty before admission", True(cache.GetImage(parent_id).flags & VideoCore::ImageFlagBits::CpuDirty));
        if (is("--parent-gpu-authority")) {
            cache.UpdateImage(parent_id);
            std::memcpy(readback.mapped_data.data() + ReadGuard, Written.data(), 8);
            readback.Flush(0, MapSize);
            const vk::BufferImageCopy region{.bufferOffset = ReadGuard,
                .imageSubresource = {vk::ImageAspectFlagBits::eColor, TargetMip, 0, 1}, .imageExtent = {2, 1, 1}};
            runtime.UploadImage(&cache.GetImage(parent_id), &readback, std::span{&region, 1});
            runtime.FlushBarriers(); scheduler.Finish();
            cache.MarkGpuWritten(parent_id);
            read_image(parent_id, TargetMip, std::vector<u32>(Written.begin(), Written.end()), "negative setup parent actual GPU write");
            RequireNegativeSetup();
            std::puts("NEGATIVE Float initial-parent-GpuModified: companion admission must stop");
        }
        command(1, false, true);
        read_parent(1, false);
    });
    if (is("--parent-gpu-authority")) { std::fputs("FAIL: Float GPU parent admitted\n", stderr); ++failures; return complete(); }
    Expect("Float no-store publication preserves entire32KiB mapping", !std::memcmp(guest, seed1.data(), MapSize));
    Expect("Float no-store was a real dispatch", no_store_dispatches == 1 && store_dispatches == 0);
    if (failures) return complete();
    on_a([&] { command(1, true, false, is("--skip-store")); });
    if (!is("--skip-store")) on_a([&] {
        read_image(child_id, 0, std::vector<u32>(Written.begin(), Written.end()), "Float actual finite store before publication");
    });
    if (failures) return complete();
    if (is("--mixed-authority")) {
        RequireNegativeSetup();
        guest[Prefix + Tail] ^= 1;
        std::puts("NEGATIVE Float mixed-authority: materialization must stop");
    }
    if (is("--skip-materialize")) {
        RequireNegativeSetup(); std::puts("MUTATION Float skip-materialize after verified store");
    } else on_a([&] { cache.MaterializeBc4AliasesAfterCommand(); cache.ProcessDownloadImages(); scheduler.Finish(); });
    if (is("--mixed-authority")) { std::fputs("FAIL: Float mixed authority accepted\n", stderr); ++failures; return complete(); }
    const auto written1 = Seed(1, true);
    Expect("Float publication changes only8B; all siblings/504padding/guards intact",
           !std::memcmp(guest, written1.data(), MapSize));
    if (is("--skip-store") || is("--skip-materialize") || failures) return complete();
    on_a([&] {
        read_parent(1, true);
        const auto tick = scheduler.CurrentTick();
        cache.MaterializeBc4AliasesAfterCommand();
        Expect("Float no-pending materialization does not submit", scheduler.CurrentTick() == tick);
        if (is("--other-image-write")) {
            const auto different = Linear(2, TargetMip);
            std::memcpy(readback.mapped_data.data() + ReadGuard, different.data(), 8);
            readback.Flush(0, MapSize);
            const vk::BufferImageCopy region{.bufferOffset = ReadGuard,
                .imageSubresource = {vk::ImageAspectFlagBits::eColor, TargetMip, 0, 1}, .imageExtent = {2, 1, 1}};
            RequireNegativeSetup();
            std::puts("NEGATIVE Float other-image-write: linked parent UploadImage must stop");
            runtime.UploadImage(&cache.GetImage(parent_id), &readback, std::span{&region, 1});
            std::fputs("FAIL: Float parent image write accepted\n", stderr); ++failures;
        }
        if (is("--raw-tail") || is("--raw-sibling") || is("--raw-full")) {
            RequireNegativeSetup();
            std::printf("NEGATIVE Float %s: protected parent raw write must stop\n", mode + 2);
            const uintptr_t address = base + (is("--raw-tail") ? Tail : 8);
            (void)rasterizer.GetBufferCache().ObtainBuffer(is("--raw-full") ? base : address,
                is("--raw-full") ? GuestSize : 8, true);
            std::fputs("FAIL: Float parent raw write accepted\n", stderr); ++failures;
        }
    });
    if (is("--raw-tail") || is("--raw-sibling") || is("--raw-full") || is("--other-image-write") || failures) return complete();
    const auto seed2 = Seed(2);
    const auto faults2 = write_faults;
    std::memcpy(guest, seed2.data(), MapSize);
    Expect("linked CPU rewrite traversed real fault", write_faults > faults2);
    on_a([&] { select_parent(); command(2, false, true); read_parent(2, false); });
    Expect("memo reuse/no-store preserves new complete CPU seed", !std::memcmp(guest, seed2.data(), MapSize));
    if (failures) return complete();
    on_a([&] { command(2, true, false); });
    Expect("VM phase leaves completed Float store unpublished", !std::memcmp(guest, seed2.data(), MapSize));
    if (failures) return complete();
    const auto old_parent = parent_id, old_child = child_id;
    const auto old_parent_uid = parent_uid, old_child_uid = child_uid;
    RuntimeMemoryAliasSnapshot before{}, after{};
    assert(runtime_memory_alias_snapshot(base, nullptr, 0, &before) == 1);
    void* alias = nullptr;
    assert(!map(&alias, MapSize, 3, 0, physical, Page));
    const auto written2 = Seed(2, true);
    Expect("Float future alias sees exact selective VM publication", !std::memcmp(alias, written2.data(), MapSize));
    Expect("Float original mapping sees exact selective VM publication", !std::memcmp(guest, written2.data(), MapSize));
    on_a([&] {
        Expect("VM mutation retired both linked identities", !cache.TryGetImage(old_parent, old_parent_uid) &&
               !cache.TryGetImage(old_child, old_child_uid));
    });
    assert(runtime_memory_alias_snapshot(base, nullptr, 0, &after) == 1);
    Expect("Float VM generation and physical alias advanced", after.generation > before.generation && after.total == 1);
    const auto seed3 = Seed(3);
    std::memcpy(alias, seed3.data(), MapSize);
    Expect("Float alias CPU write visible across full original mapping", !std::memcmp(guest, seed3.data(), MapSize));
    on_a([&] { select_parent(); command(3, false, true); read_parent(3, false); });
    if (failures) return complete();
    const auto prior_parent = parent_id, prior_child = child_id;
    const auto prior_parent_uid = parent_uid, prior_child_uid = child_uid;
    assert(!release(physical, MapSize));
    int64_t replacement = -1;
    assert(!allocate(physical, physical + MapSize, MapSize, Page, 0, &replacement));
    assert(replacement == physical);
    void* same_va = mapped;
    assert(!map(&same_va, MapSize, 3, 0x10, replacement, Page));
    assert(same_va == mapped);
    const auto seed4 = Seed(4);
    std::memcpy(guest, seed4.data(), MapSize);
    on_a([&] {
        Expect("same-VA reuse cannot reuse either old linked identity",
               !cache.TryGetImage(prior_parent, prior_parent_uid) && !cache.TryGetImage(prior_child, prior_child_uid));
        select_parent(); command(4, false, true); read_parent(4, false);
    });
    Expect("same-VA new Float relation preserves entire new seed", !std::memcmp(guest, seed4.data(), MapSize));
    assert(runtime_memory_alias_snapshot(base, nullptr, 0, &before) == 1);
    Expect("Float same-size reuse has new mapping generation", before.generation > after.generation);
    std::printf("Float dispatches no-store=%u store=%u; publication via production cache only\n",
                no_store_dispatches, store_dispatches);
    return complete();
}
} // namespace FloatCache
#endif

int main(int argc, char** argv) {
    const char* mode = argc == 2 ? argv[1] : "ordinary";
    const auto is = [&](const char* value) { return !std::strcmp(mode, value); };
#ifdef BC4_CACHE_FLOAT_TAIL_PROBE
    if (argc > 2 || (!is("ordinary") && !is("--skip-store") && !is("--skip-materialize") &&
                    !is("--mixed-authority") && !is("--raw-tail") && !is("--raw-sibling") &&
                    !is("--raw-full") && !is("--parent-gpu-authority") && !is("--other-image-write"))) return 2;
    setenv("BB_READBACK_LINEAR", "1", 1);
    constexpr unsigned AllocationBytes = FloatCache::MapSize;
#else
    if (argc > 2 || (!is("ordinary") && !is("--skip-store") &&
                    !is("--skip-materialize") && !is("--raw-interior") &&
                    !is("--raw-full") && !is("--mixed-authority"))) return 2;
    constexpr unsigned AllocationBytes = Page;
#endif
    test_alarm(180);
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
#ifdef BC4_CACHE_FLOAT_TAIL_PROBE
    std::printf("BC4 Float-tail actual cache/VM fixture mode=%s\n", mode);
#else
    std::printf("BC4 actual cache/VM fixture mode=%s\n", mode);
#endif
    setenv("BB_UPSCALER", "none", 1);
    const BbGpuConfig config{.title = "BC4 cache/VM fixture", .serial = "BC4_CACHE_FIXTURE"};
    Core::Emulator::FillElfInfo(config);
    if (Common::ElfInfo::Instance().GameSerial() != config.serial ||
        Common::ElfInfo::Instance().Title() != config.title) {
        std::fputs("SETUP FAIL: fixture game metadata mismatch\n", stderr);
        return 2;
    }
    std::printf("fixture metadata initialized: serial=%s\n", config.serial);
    runtime_memory_reserve();
    struct sigaction action{};
    action.sa_sigaction = Fault;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    assert(!sigaction(SIGBUS, &action, nullptr));
    assert(!sigaction(SIGSEGV, &action, nullptr));
    using Allocate = int32_t (ABI *)(int64_t, int64_t, uint64_t, uint64_t, int, int64_t*);
    using Map = int32_t (ABI *)(void**, uint64_t, int, int, int64_t, uint64_t);
    using Release = int32_t (ABI *)(int64_t, uint64_t);
    const auto allocate = reinterpret_cast<Allocate>(runtime_memory_resolve("sceKernelAllocateDirectMemory"));
    const auto map = reinterpret_cast<Map>(runtime_memory_resolve("sceKernelMapDirectMemory"));
    const auto release = reinterpret_cast<Release>(runtime_memory_resolve("sceKernelReleaseDirectMemory"));
    assert(allocate && map && release);
    int64_t physical = -1;
    assert(!allocate(0, INT64_MAX, AllocationBytes, Page, 0, &physical));
    void* mapped = nullptr;
    assert(!map(&mapped, AllocationBytes, 3, 0, physical, Page));
#ifndef BC4_CACHE_FLOAT_TAIL_PROBE
    auto* guest = static_cast<unsigned char*>(mapped);
    const uintptr_t address = uintptr_t(guest) + Offset;
    std::memset(guest, 0xa5, Page);
    std::memcpy(guest + Offset, Tiled(Seed(0)).data(), Bytes);
    Expect("fixed independent tile coordinate", Tiled(Seed(1, true))[18] == WriteLo);
    Expect("tiled oracle distinguishes raw linear publication", Tiled(Seed(1, true)) != Seed(1, true));

#endif

    Vulkan::Instance instance(0, false);
    Vulkan::Scheduler scheduler(instance);
    Vulkan::Runtime runtime(instance, scheduler);
    auto liverpool = std::make_unique<AmdGpu::Liverpool>();
    auto rasterizer = std::make_unique<Vulkan::Rasterizer>(instance, scheduler, runtime, liverpool.get());
#ifndef BC4_CACHE_FLOAT_TAIL_PROBE
    auto& cache = rasterizer->GetTextureCache();
    const auto on_a = [&](auto&& call) { liverpool->SendCommand<true>(std::forward<decltype(call)>(call)); };
#endif
    vk::detail::DynamicLoader loader;
    vk::detail::DispatchLoaderDynamic d;
    d.init(loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr"));
    d.init(instance.GetInstance()); d.init(instance.GetDevice());
#ifndef BC4_CACHE_FLOAT_TAIL_PROBE
    const auto device = instance.GetDevice();
#endif
    const auto properties = instance.GetPhysicalDevice().getProperties(d);
    std::printf("device=%s vendor=%#x driver=%#x\n", properties.deviceName.data(),
                properties.vendorID, properties.driverVersion);
    const auto shutdown = [&] {
        runtime_memory_set_gpu_lifetime_hooks(nullptr, nullptr, nullptr);
        runtime_memory_set_gpu_hooks(nullptr, nullptr, nullptr);
        liverpool.reset();
        scheduler.Finish();
        scheduler.Close();
        runtime.SetImageAccessHook(nullptr, nullptr);
        scheduler.SetSubmitCallback({});
        rasterizer.reset();
        // All cache producers are gone; stop the persistent IO worker while its
        // synchronization globals are still alive, before normal static teardown.
        Expect("cache storage initialized before explicit close", Storage::DataBase::Instance().IsOpened());
        Storage::DataBase::Instance().Close();
        Expect("cache storage worker explicitly closed", !Storage::DataBase::Instance().IsOpened());
        test_alarm(0);
    };
#ifdef BC4_CACHE_FLOAT_TAIL_PROBE
    const auto transfer = vk::FormatFeatureFlagBits2::eTransferSrc | vk::FormatFeatureFlagBits2::eTransferDst;
    Expect("Float cache device format support",
           instance.IsFormatSupported(vk::Format::eBc4UnormBlock, transfer) &&
           instance.IsFormatSupported(vk::Format::eR32G32Sfloat, transfer | vk::FormatFeatureFlagBits2::eStorageImage) &&
           instance.GetPhysicalDevice().getFeatures(d).shaderStorageImageExtendedFormats);
    if (failures) { assert(!release(physical, AllocationBytes)); shutdown(); return 2; }
    return FloatCache::Run(instance, scheduler, runtime, *liverpool, *rasterizer, d,
                           mapped, physical, mode, shutdown);
#else
    const auto transfer = vk::FormatFeatureFlagBits2::eTransferSrc | vk::FormatFeatureFlagBits2::eTransferDst;
    assert(instance.IsFormatSupported(vk::Format::eBc4UnormBlock, transfer));
    assert(instance.IsFormatSupported(vk::Format::eR32G32Uint, transfer | vk::FormatFeatureFlagBits2::eStorageImage));
    const vk::DescriptorSetLayoutBinding binding{.binding = 0,
        .descriptorType = vk::DescriptorType::eStorageImage, .descriptorCount = 1,
        .stageFlags = vk::ShaderStageFlagBits::eCompute};
    auto descriptors = Require(device.createDescriptorSetLayoutUnique({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = 1, .pBindings = &binding}, nullptr, d));
    auto layout = Require(device.createPipelineLayoutUnique({
        .setLayoutCount = 1, .pSetLayouts = &*descriptors}, nullptr, d));
    const auto code = StoreShader();
    auto module = Require(device.createShaderModuleUnique({.codeSize = code.size() * sizeof(u32),
        .pCode = code.data()}, nullptr, d));
    auto pipeline = Require(device.createComputePipelineUnique({}, {
        .stage = {.stage = vk::ShaderStageFlagBits::eCompute, .module = *module, .pName = "main"},
        .layout = *layout}, nullptr, d));
    VideoCore::Buffer readback(instance, 0, Page, VideoCore::MemoryType::HostCached);
    auto bc = Desc(address, true), integer = Desc(address, false);
    assert(!VideoCore::Bc4Alias::RejectLayout(bc.info, integer.info));
    assert(bc.info.guest_size == Bytes && integer.info.guest_size == Bytes);
    for (const auto* info : {&bc.info, &integer.info}) {
        assert(info->props.is_tiled && info->tile_mode == AmdGpu::TileMode::Thin1DThin &&
               info->array_mode == AmdGpu::ArrayMode::Array1DTiledThin1 && info->num_bits == 64 &&
               info->resources.levels == 1 && info->resources.layers == 1 &&
               info->mips_layout[0].offset == 0 && info->mips_layout[0].size == Bytes &&
               info->mips_layout[0].pitch == info->size.width &&
               info->mips_layout[0].height == info->size.height);
    }
    VideoCore::ImageId image_id{};
    u64 image_uid = 0;
    const auto read_image = [&](VideoCore::Image& image, const Payload& expected) {
        std::memset(readback.mapped_data.data(), 0xa5, Page);
        readback.Flush(0, Page);
        const vk::BufferImageCopy region{.bufferOffset = Offset,
            .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .imageExtent = {image.info.size.width, image.info.size.height, 1}};
        runtime.DownloadImage(&image, &readback, std::span{&region, 1});
        runtime.FlushBarriers();
        scheduler.Record([&](vk::CommandBuffer cmd) {
            const vk::MemoryBarrier2 barrier{.srcStageMask = vk::PipelineStageFlagBits2::eCopy,
                .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eHost,
                .dstAccessMask = vk::AccessFlagBits2::eHostRead};
            cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &barrier}, d);
        });
        scheduler.Finish();
        readback.Invalidate(0, Page);
        Expect("actual image bytes equal full independent linear oracle",
               Matches(readback.mapped_data.data() + Offset, expected));
        Expect("GPU readback canaries", Canaries(readback.mapped_data.data()));
    };
    const auto select = [&](Cache::ImageDesc desc) {
        image_id = cache.FindImage(desc);
        cache.FindTexture(image_id, desc);
        image_uid = cache.GetImage(image_id).image_uid;
    };
    const auto store = [&] {
        select(integer);
        auto& image = cache.GetImage(image_id);
        assert(image.bc4_integer_alias);
        auto view = Require(device.createImageViewUnique({.image = image.GetImage(),
            .viewType = vk::ImageViewType::e2D, .format = vk::Format::eR32G32Uint,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}}, nullptr, d));
        runtime.Transit(&image, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                        vk::AccessFlagBits2::eShaderStorageWrite);
        runtime.FlushBarriers();
        if (!is("--skip-store")) scheduler.Record([&](vk::CommandBuffer cmd) {
            const vk::DescriptorImageInfo info{.imageView = *view, .imageLayout = vk::ImageLayout::eGeneral};
            const vk::WriteDescriptorSet write{.dstBinding = 0, .descriptorCount = 1,
                .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &info};
            cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline, d);
            cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *layout, 0, write, d);
            cmd.dispatch(1, 1, 1, d);
        });
        else {
            RequireNegativeSetup();
            std::puts("MUTATION skip-store");
        }
        scheduler.Finish(); // also holds the descriptor view through GPU completion
    };
    on_a([&] { select(bc); read_image(cache.GetImage(image_id), Seed(0)); });
    const auto faults_before = write_faults;
    std::memcpy(guest + Offset, Tiled(Seed(1)).data(), Bytes);
    Expect("CPU rewrite traversed actual tracked write fault", write_faults > faults_before);
    on_a([&] {
        Expect("old cached BC4 became CPU dirty", True(cache.GetImage(image_id).flags & VideoCore::ImageFlagBits::CpuDirty));
        select(integer);
        read_image(cache.GetImage(image_id), Seed(1));
        store();
        if (!is("--skip-materialize") && !is("--mixed-authority")) cache.MaterializeBc4AliasesAfterCommand();
        else if (is("--skip-materialize")) {
            read_image(cache.GetImage(image_id), Seed(1, true));
            RequireNegativeSetup();
            std::puts("MUTATION skip-materialize");
        }
    });
    if (is("--mixed-authority")) {
        RequireNegativeSetup();
        guest[Offset] ^= 1;
        std::puts("NEGATIVE mixed-authority: next production materialization must stop");
        on_a([&] { cache.MaterializeBc4AliasesAfterCommand(); });
        std::fputs("FAIL: mixed authority accepted\n", stderr);
        _exit(1);
    }
    Expect("command completion publishes exact tiled bytes", Matches(guest + Offset, Tiled(Seed(1, true))));
    Expect("publication preserves full guest page canaries", Canaries(guest));
    if (is("--skip-store") || is("--skip-materialize")) {
        // End this one deliberate bypass after its oracle; do not enter a second mutation
        // with a failed first oracle and misclassify that as a setup failure.
        assert(!release(physical, Page));
        shutdown();
        std::printf("bc4-cache-alias bypass: %s (%u failures)\n",
                    failures ? "FAIL as expected" : "UNEXPECTED PASS", failures);
        return failures ? 1 : 0;
    }
    on_a([&] {
        if (is("--raw-interior") || is("--raw-full")) {
            RequireNegativeSetup();
            std::puts("NEGATIVE converted raw write: next production ObtainBuffer must stop");
            (void)rasterizer->GetBufferCache().ObtainBuffer(address + (is("--raw-interior") ? 8 : 0),
                is("--raw-interior") ? 8 : Bytes, true);
            std::fputs("FAIL: raw write accepted\n", stderr);
            _exit(1);
        }
        const auto tick = scheduler.CurrentTick();
        cache.MaterializeBc4AliasesAfterCommand();
        Expect("no-pending materialization does not submit", scheduler.CurrentTick() == tick);
        select(bc);
        read_image(cache.GetImage(image_id), Seed(1, true));
    });

    // A new physical alias must see publication before its map is granted. Leave this
    // converted image GPU authoritative; the real VM begin hook must publish and evict it.
    std::memcpy(guest + Offset, Tiled(Seed(2)).data(), Bytes);
    on_a([&] {
        store();
        Expect("VM-boundary arm retains pending GPU authority",
               True(cache.GetImage(image_id).flags & VideoCore::ImageFlagBits::GpuModified));
    });
    Expect("VM-boundary arm has not prematurely published the storage write",
           Matches(guest + Offset, Tiled(Seed(2))));
    const auto old_id = image_id;
    const auto old_uid = image_uid;
    RuntimeMemoryAliasSnapshot before{}, after{};
    assert(runtime_memory_alias_snapshot(address, nullptr, 0, &before) == 1);
    void* alias = nullptr;
    assert(!map(&alias, Page, 3, 0, physical, Page));
    Expect("future alias sees exact VM-boundary publication", Matches(static_cast<u8*>(alias) + Offset, Tiled(Seed(2, true))));
    Expect("original view sees VM-boundary publication", Matches(guest + Offset, Tiled(Seed(2, true))));
    assert(runtime_memory_alias_snapshot(address, nullptr, 0, &after) == 1);
    Expect("mapping generation changed and physical alias is recorded", after.generation > before.generation && after.total == 1);
    on_a([&] { Expect("VM hook evicted old converted identity", !cache.TryGetImage(old_id, old_uid)); });
    std::memcpy(static_cast<u8*>(alias) + Offset, Tiled(Seed(3)).data(), Bytes);
    Expect("future alias write visible in original backing", Matches(guest + Offset, Tiled(Seed(3))));
    on_a([&] { select(bc); read_image(cache.GetImage(image_id), Seed(3)); });
    Expect("both aliases preserve canaries", Canaries(guest) && Canaries(static_cast<u8*>(alias)));
    const auto prior_id = image_id;
    const auto prior_uid = image_uid;
    assert(!release(physical, Page)); // retires both guest aliases and their imported arenas
    int64_t replacement = -1;
    assert(!allocate(physical, physical + Page, Page, Page, 0, &replacement));
    assert(replacement == physical);
    void* same_va = mapped;
    assert(!map(&same_va, Page, 3, 0x10, replacement, Page));
    assert(same_va == mapped);
    std::memset(guest, 0xa5, Page);
    std::memcpy(guest + Offset, Tiled(Seed(4)).data(), Bytes);
    on_a([&] {
        Expect("same-VA replacement cannot reuse old image identity", !cache.TryGetImage(prior_id, prior_uid));
        select(bc); read_image(cache.GetImage(image_id), Seed(4));
    });
    assert(runtime_memory_alias_snapshot(address, nullptr, 0, &before) == 1);
    Expect("same-VA same-size physical reuse has a new mapping generation", before.generation > after.generation);
    assert(!release(replacement, Page));
    shutdown();
    std::printf("bc4-cache-alias-test: %s (%u failures); completed cache/VM operations only\n",
                failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
#endif
}
