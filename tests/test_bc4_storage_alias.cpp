// SPDX-License-Identifier: GPL-2.0-or-later
// BC4 storage alias (the PREVIEW-28p Status-screen stop): a shader writes an R32G32Uint T# of the
// block grid at the address of a cached BC4 image. Through the production pieces of the texture
// cache's path, without a guest process:
//   - ImageInfo/ImageViewInfo built from the two T#s (as BindTextures' ImageDesc builds them);
//   - ResolveOverlap's equal-address decision (ClassifyEqualAddressOverlap, which ResolveOverlap
//     calls);
//   - in a child forked before any Vulkan or thread initialization: a BC4 VideoCore::Image
//     created on the device (image creation drops its storage usage), then the storage view of it
//     (Image::FindView, the ImageView constructor's stop).
// A synthetic reproduction of the route (an R32G32Uint T# over a BC4 image), not proof of the live
// stopped T#. FindImage's lookup itself needs guest memory (RefreshImage) and is not run here.
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <chrono>
#include <cstdio>
#include <string>
#include "common/slot_vector.h"
#include "shader_recompiler/resource.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/blit_helper.h" // Runtime destroys its BlitHelper
#include "video_core/texture_cache/host_compatibility.h"
#include "video_core/texture_cache/image.h"
#include "video_core/texture_cache/image_view.h"
#include "video_core/texture_cache/texture_cache.h"

namespace {
unsigned failures = 0;
void Expect(const std::string& what, bool ok) {
    std::printf("%-86s %s\n", what.c_str(), ok ? "ok" : "FAIL");
    failures += ok ? 0 : 1;
}

struct ChildResult {
    bool forked = false, waited = false, timed_out = false;
    int status = 0;
    std::string stderr_text;
};

/// Runs `body` in a forked child with stderr captured. One deadline bounds reading and reaping: a
/// child still alive at it (silent, or with stderr closed) is killed and reported as timed out.
template <typename Body>
ChildResult RunChild(Body body, std::chrono::seconds limit = std::chrono::seconds(120)) {
    ChildResult result;
    int pipe_fds[2];
    if (pipe(pipe_fds) != 0) {
        return result;
    }
    std::fflush(stdout);
    std::fflush(stderr);
    const pid_t child = fork();
    if (child < 0) {
        return result;
    }
    if (child == 0) {
        if (dup2(pipe_fds[1], STDERR_FILENO) != STDERR_FILENO) {
            std::_Exit(98); // no stderr to report on
        }
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        body();
        std::_Exit(0); // not reached when the body stops
    }
    result.forked = true;
    close(pipe_fds[1]);
    const auto deadline = std::chrono::steady_clock::now() + limit;
    char chunk[4096];
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                              deadline - std::chrono::steady_clock::now())
                              .count();
        pollfd fd{pipe_fds[0], POLLIN, 0};
        if (left <= 0 || poll(&fd, 1, int(left)) <= 0) {
            break; // the deadline: reaped (and killed) below
        }
        const ssize_t n = read(pipe_fds[0], chunk, sizeof(chunk));
        if (n <= 0) {
            break;
        }
        result.stderr_text.append(chunk, size_t(n));
    }
    close(pipe_fds[0]);
    // Reap within the same deadline: stderr may be closed while the child lives on.
    for (;;) {
        const pid_t done = waitpid(child, &result.status, WNOHANG);
        if (done == child) {
            result.waited = true;
            return result;
        }
        if (done < 0 || std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        usleep(10000);
    }
    kill(child, SIGKILL);
    result.timed_out = true;
    result.waited = waitpid(child, &result.status, 0) == child; // after SIGKILL: prompt
    return result;
}

/// A linear 2D T# with one level and one layer.
AmdGpu::Image Sharp(VAddr address, AmdGpu::DataFormat data, AmdGpu::NumberFormat number,
                    u32 width, u32 height) {
    AmdGpu::Image t{};
    t.base_address = address >> 8;
    t.data_format = static_cast<u64>(data);
    t.num_format = static_cast<u64>(number);
    t.width = width - 1;
    t.height = height - 1;
    t.pitch = width - 1;
    t.dst_sel_x = 4; // X, Y, Z, W
    t.dst_sel_y = 5;
    t.dst_sel_z = 6;
    t.dst_sel_w = 7;
    t.tiling_index = static_cast<u64>(AmdGpu::TileMode::DisplayLinearAligned);
    t.type = static_cast<u64>(AmdGpu::ImageType::Color2D);
    return t;
}
} // namespace

int main() {
    // The fixture's own bound (before any Vulkan): a child that closes stderr and stays alive is
    // killed at the deadline, not waited for.
    const ChildResult silent = RunChild(
        [] {
            close(STDERR_FILENO);
            for (;;) {
                pause();
            }
        },
        std::chrono::seconds(2));
    Expect("fixture: a child with stderr closed that stays alive is killed at the deadline",
           silent.forked && silent.timed_out && silent.waited && WIFSIGNALED(silent.status) &&
               WTERMSIG(silent.status) == SIGKILL);

    constexpr VAddr Address = 0x70e5c3af00; // the stop's address (any aligned address works)
    constexpr u32 Size = 256;               // BC4 texels; the block grid is 64 x 64
    const AmdGpu::Image bc4 = Sharp(Address, AmdGpu::DataFormat::FormatBc4,
                                    AmdGpu::NumberFormat::Unorm, Size, Size);
    const AmdGpu::Image grid = Sharp(Address, AmdGpu::DataFormat::Format32_32,
                                     AmdGpu::NumberFormat::Uint, Size / 4, Size / 4);
    Shader::ImageResource sampled{};
    Shader::ImageResource written{};
    written.is_written = true;

    const VideoCore::ImageInfo cached{bc4, sampled};
    const VideoCore::ImageInfo request{grid, written};
    std::printf("cached %s %ux%u guest %#llx+%#llx; request %s %ux%u guest %#llx+%#llx\n",
                vk::to_string(cached.pixel_format).c_str(), cached.size.width,
                cached.size.height, (unsigned long long)cached.guest_address,
                (unsigned long long)cached.guest_size,
                vk::to_string(request.pixel_format).c_str(), request.size.width,
                request.size.height, (unsigned long long)request.guest_address,
                (unsigned long long)request.guest_size);
    Expect("the cached image is BC4, the request R32G32Uint",
           cached.pixel_format == vk::Format::eBc4UnormBlock &&
               request.pixel_format == vk::Format::eR32G32Uint);
    Expect("same guest address and size", cached.guest_address == request.guest_address &&
                                              cached.guest_size == request.guest_size);
    Expect("same block dimensions and bits per block (no DifferentShape)",
           cached.BlockDim() == request.BlockDim() && cached.num_bits == request.num_bits);
    Expect("BC4 and R32G32Uint are view-compatible formats",
           VideoCore::IsVulkanFormatCompatible(cached.pixel_format, request.pixel_format));
    Expect("ResolveOverlap: not a different shape (no pool-reuse drop)",
           !VideoCore::EqualAddressDifferentShape(request, cached));
    Expect("ResolveOverlap's equal-address decision: a view (NeedsView)",
           VideoCore::ClassifyEqualAddressOverlap(request, cached) ==
               VideoCore::EqualAddressOverlap::NeedsView);

    // The device part runs in a child forked before any Vulkan or thread initialization: it
    // reports its checks on stderr, then requests the storage view, which stops it.
    const ChildResult stop = RunChild([&] {
        Vulkan::Instance instance(0, false);
        Vulkan::Scheduler scheduler(instance, true);
        Vulkan::Runtime runtime(instance, scheduler);
        Common::SlotVector<VideoCore::ImageView> views;
        VideoCore::Image image{instance, runtime, views, cached};
        const auto report = [](const char* name, bool ok) {
            std::fprintf(stderr, "CHILD %s: %s\n", name, ok ? "ok" : "FAIL");
        };
        report("BC4 image without storage usage",
               !(image.usage_flags & vk::ImageUsageFlagBits::eStorage));
        const VideoCore::ImageViewInfo view_info{grid, written};
        report("R32G32Uint storage view requested",
               view_info.is_storage && view_info.format == vk::Format::eR32G32Uint);
        report("StorageUsageMissing refuses it",
               VideoCore::StorageUsageMissing(view_info, image.usage_flags));
        std::fflush(stderr);
        (void)image.FindView(view_info, false);
    });
    std::printf("child stderr:\n%s", stop.stderr_text.c_str());
    Expect("the child was forked, not timed out, and reaped",
           stop.forked && !stop.timed_out && stop.waited);
    Expect("the BC4 image was created without storage usage",
           stop.stderr_text.find("CHILD BC4 image without storage usage: ok") != std::string::npos);
    Expect("the requested view is a storage view in R32G32Uint",
           stop.stderr_text.find("CHILD R32G32Uint storage view requested: ok") !=
               std::string::npos);
    Expect("StorageUsageMissing refuses it",
           stop.stderr_text.find("CHILD StorageUsageMissing refuses it: ok") != std::string::npos);
    Expect("the storage view stops (exit 23), as in PREVIEW-28p",
           WIFEXITED(stop.status) && WEXITSTATUS(stop.status) == 23);
    Expect("  naming the BC4 backing",
           stop.stderr_text.find("storage view of format Bc4UnormBlock image at 0x70e5c3af00") !=
               std::string::npos);
    Expect("  and the requested R32G32Uint storage view",
           stop.stderr_text.find("Requested view: format R32G32Uint") != std::string::npos);

    std::printf("%s: %u failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
