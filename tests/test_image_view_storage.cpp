// SPDX-License-Identifier: GPL-2.0-or-later
// Storage-view classification regression (macOS hard error in ImageView).
//
// The depth-attachment ImageViewInfo used to set is_storage for depth-write-enabled targets, so the
// Apple "storage view of an image without storage usage" check aborted on every written D32S8
// depth attachment (image creation correctly drops storage usage for that format). A written depth
// attachment is is_depth_write, not a storage view; a real storage view of such an image must still
// be refused. Runs the production ImageViewInfo constructor and the predicate ImageView asserts on.
#include <cstdio>
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/texture_cache/image_view.h"

namespace {
u32 failures = 0;
void Expect(const char* what, bool ok) {
    std::fprintf(stderr, "%-66s %s\n", what, ok ? "ok" : "FAIL");
    failures += ok ? 0 : 1;
}
} // namespace

int main() {
    using VideoCore::ImageViewInfo;
    using VideoCore::StorageUsageMissing;
    constexpr auto sampled_only = vk::ImageUsageFlagBits::eSampled |
                                  vk::ImageUsageFlagBits::eDepthStencilAttachment |
                                  vk::ImageUsageFlagBits::eTransferSrc;
    constexpr auto with_storage = sampled_only | vk::ImageUsageFlagBits::eStorage;

    for (const bool write : {true, false}) {
        AmdGpu::DepthBuffer depth{};
        depth.z_info.format = AmdGpu::DepthBuffer::ZFormat::Z32Float;
        depth.stencil_info.format = AmdGpu::DepthBuffer::StencilFormat::Stencil8;
        AmdGpu::DepthView view{};
        AmdGpu::DepthControl ctl{};
        ctl.depth_enable = 1;
        ctl.depth_write_enable = write ? 1 : 0;
        const ImageViewInfo info{depth, view, ctl};
        std::fprintf(stderr, "depth attachment, depth writes %s, format %s\n", write ? "on" : "off",
                     vk::to_string(info.format).c_str());
        Expect("  format is D32_SFLOAT_S8_UINT", info.format == vk::Format::eD32SfloatS8Uint);
        Expect("  not a storage view", !info.is_storage);
        Expect("  is_depth_write follows DB_DEPTH_CONTROL", info.is_depth_write == write);
        Expect("  no storage-usage error on an image without storage usage",
               !StorageUsageMissing(info, sampled_only));
    }

    ImageViewInfo storage{};
    storage.format = vk::Format::eR32Sfloat;
    storage.is_storage = true;
    std::fprintf(stderr, "storage descriptor view\n");
    Expect("  refused on an image without storage usage", StorageUsageMissing(storage, sampled_only));
    Expect("  accepted on an image with storage usage", !StorageUsageMissing(storage, with_storage));

    ImageViewInfo sampled{};
    sampled.format = vk::Format::eR32Sfloat;
    Expect("sampled view: no storage usage needed", !StorageUsageMissing(sampled, sampled_only));

    std::fprintf(stderr, "%s: %u failures\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
