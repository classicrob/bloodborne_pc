// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: per-use checks for device features the guest state may need (docs/macos/feature-audit.md).
// Each returns true when the guest state needs a feature the device lacks and skipping it would
// change the result; the caller turns that into a hard error on Apple. Kept pure so
// tests/test_feature_checks.cpp exercises exactly what production evaluates.
#pragma once

#include "common/types.h"
#include "video_core/amdgpu/regs_color.h"
#include "video_core/amdgpu/regs_primitive.h"
#include "video_core/amdgpu/regs_vertex.h"

namespace Vulkan::FeatureChecks {

/// A colour logic op other than copy, with at least one colour target, needs logicOp.
[[nodiscard]] constexpr bool LogicOpUnsupported(AmdGpu::ColorControl::LogicOp op,
                                                u32 num_color_attachments, bool supported) {
    return !supported && op != AmdGpu::ColorControl::LogicOp::Copy && num_color_attachments > 0;
}

/// Skipping the depth bounds test is exact only when it would always pass: no depth buffer, or
/// bounds covering [0, 1] (stored depth is clamped to that range).
[[nodiscard]] constexpr bool DepthBoundsUnsupported(bool enabled, bool depth_valid, float min,
                                                    float max, bool supported) {
    return !supported && enabled && depth_valid && !(min <= 0.0f && max >= 1.0f);
}

/// An indirect draw that takes its count from GPU memory needs drawIndirectCount.
[[nodiscard]] constexpr bool DrawIndirectCountUnsupported(u64 count_address, bool supported) {
    return !supported && count_address != 0;
}

/// True when lines are rasterized: line primitives, or polygons drawn in line mode.
[[nodiscard]] constexpr bool RasterizesLines(AmdGpu::PrimitiveType prim,
                                             AmdGpu::PolygonMode poly_mode) {
    using AmdGpu::PrimitiveType;
    return prim == PrimitiveType::LineList || prim == PrimitiveType::LineStrip ||
           prim == PrimitiveType::LineLoop || prim == PrimitiveType::AdjLineList ||
           prim == PrimitiveType::AdjLineStrip || poly_mode == AmdGpu::PolygonMode::Line;
}

/// Lines wider than one pixel need wideLines; the width of other draws has no effect.
[[nodiscard]] constexpr bool WideLinesUnsupported(AmdGpu::PrimitiveType prim,
                                                  AmdGpu::PolygonMode poly_mode, float width,
                                                  bool supported) {
    return !supported && RasterizesLines(prim, poly_mode) && width != 1.0f;
}

/// A multisampled storage descriptor view needs shaderStorageImageMultisample. Depth
/// attachments are not storage views (ImageViewInfo::is_depth_write).
[[nodiscard]] constexpr bool StorageMultisampleUnsupported(bool is_storage_view, u32 samples,
                                                           bool supported) {
    return !supported && is_storage_view && samples > 1;
}

} // namespace Vulkan::FeatureChecks
