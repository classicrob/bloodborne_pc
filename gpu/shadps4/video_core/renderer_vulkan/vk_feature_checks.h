// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: per-use checks for device features the guest state may need (docs/macos/feature-audit.md).
// Each returns true when the guest state needs a feature the device lacks and skipping it would
// change the result; the caller turns that into a hard error on Apple. Kept pure so
// tests/test_feature_checks.cpp exercises exactly what production evaluates.
#pragma once

#include "common/types.h"
#include "video_core/amdgpu/pixel_format.h"
#include "video_core/amdgpu/regs_color.h"
#include "video_core/amdgpu/regs_primitive.h"
#include "video_core/amdgpu/regs_vertex.h"

namespace Vulkan::FeatureChecks {

/// Whether a colour target is affected by the logic op: bound, written, and of a normalized or
/// integer format. Vulkan (and GCN's CB) ignore the logic op for float and sRGB targets.
[[nodiscard]] constexpr bool LogicOpAppliesTo(AmdGpu::DataFormat data_format,
                                              AmdGpu::NumberFormat num_format, u32 write_mask) {
    return data_format != AmdGpu::DataFormat::FormatInvalid && write_mask != 0 &&
           num_format != AmdGpu::NumberFormat::Float && num_format != AmdGpu::NumberFormat::Srgb;
}

/// A colour logic op other than copy, on a target it applies to, needs logicOp.
[[nodiscard]] constexpr bool LogicOpUnsupported(AmdGpu::ColorControl::LogicOp op,
                                                bool applies_to_a_target, bool supported) {
    return !supported && op != AmdGpu::ColorControl::LogicOp::Copy && applies_to_a_target;
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

enum class RasterPrimitive { Points, Lines, Triangles };

/// The primitive class reaching the rasterizer: the geometry shader's output when one runs,
/// else the tessellator's output topology, else the input primitive type.
[[nodiscard]] constexpr RasterPrimitive RasterizedPrimitive(
    AmdGpu::PrimitiveType prim, bool geometry_stage, AmdGpu::GsOutputPrimitiveType gs_output,
    bool tessellation, AmdGpu::TessellationTopology tess_topology) {
    using AmdGpu::PrimitiveType;
    if (geometry_stage) {
        return gs_output == AmdGpu::GsOutputPrimitiveType::PointList   ? RasterPrimitive::Points
               : gs_output == AmdGpu::GsOutputPrimitiveType::LineStrip ? RasterPrimitive::Lines
                                                                       : RasterPrimitive::Triangles;
    }
    if (tessellation) {
        return tess_topology == AmdGpu::TessellationTopology::Point  ? RasterPrimitive::Points
               : tess_topology == AmdGpu::TessellationTopology::Line ? RasterPrimitive::Lines
                                                                     : RasterPrimitive::Triangles;
    }
    switch (prim) {
    case PrimitiveType::PointList:
        return RasterPrimitive::Points;
    case PrimitiveType::LineList:
    case PrimitiveType::LineStrip:
    case PrimitiveType::LineLoop:
    case PrimitiveType::AdjLineList:
    case PrimitiveType::AdjLineStrip:
        return RasterPrimitive::Lines;
    default:
        return RasterPrimitive::Triangles;
    }
}

/// True when lines are rasterized: line primitives, or triangles with either face in line mode.
[[nodiscard]] constexpr bool RasterizesLines(RasterPrimitive raster, bool polygon_mode_enabled,
                                             AmdGpu::PolygonMode front, AmdGpu::PolygonMode back) {
    return raster == RasterPrimitive::Lines ||
           (raster == RasterPrimitive::Triangles && polygon_mode_enabled &&
            (front == AmdGpu::PolygonMode::Line || back == AmdGpu::PolygonMode::Line));
}

/// Lines of any width other than one pixel need wideLines (a width below one is not known to
/// match either); the width of other draws has no effect.
[[nodiscard]] constexpr bool WideLinesUnsupported(bool rasterizes_lines, float width,
                                                  bool supported) {
    return !supported && rasterizes_lines && width != 1.0f;
}

/// GCN has separate front and back polygon modes; Vulkan has one (the front mode is used).
/// Visible back faces in a different mode cannot be drawn.
[[nodiscard]] constexpr bool PolygonModesDiffer(RasterPrimitive raster, bool polygon_mode_enabled,
                                                AmdGpu::PolygonMode front,
                                                AmdGpu::PolygonMode back, bool cull_back) {
    return raster == RasterPrimitive::Triangles && polygon_mode_enabled && front != back &&
           !cull_back;
}

/// A multisampled storage descriptor view needs shaderStorageImageMultisample. Depth
/// attachments are not storage views (ImageViewInfo::is_depth_write).
[[nodiscard]] constexpr bool StorageMultisampleUnsupported(bool is_storage_view, u32 samples,
                                                           bool supported) {
    return !supported && is_storage_view && samples > 1;
}

} // namespace Vulkan::FeatureChecks
