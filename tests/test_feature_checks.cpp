// SPDX-License-Identifier: GPL-2.0-or-later
// Per-use feature checks (vk_feature_checks.h), both sides: each must stay silent on the
// legitimate state next to it (feature present, or skipping it is exact) and fire on the state the
// device cannot honour. Production evaluates these same functions before its macOS hard errors.
#include <cstdio>
#include "video_core/renderer_vulkan/vk_feature_checks.h"

namespace {
u32 failures = 0;
void Expect(const char* what, bool fires, bool want) {
    const bool ok = fires == want;
    std::fprintf(stderr, "%-62s %-6s %s\n", what, fires ? "FIRES" : "silent", ok ? "ok" : "FAIL");
    failures += ok ? 0 : 1;
}
} // namespace

int main() {
    using namespace Vulkan::FeatureChecks;
    using AmdGpu::PolygonMode;
    using AmdGpu::PrimitiveType;
    using Op = AmdGpu::ColorControl::LogicOp;
    using AmdGpu::DataFormat;
    using AmdGpu::NumberFormat;
    using AmdGpu::TessellationTopology;
    using Gs = AmdGpu::GsOutputPrimitiveType;
    constexpr auto Rgba8 = DataFormat::Format8_8_8_8;

    std::fprintf(stderr, "logicOp targets\n");
    Expect("  unbound target (CB disabled or MRT masked)",
           LogicOpAppliesTo(DataFormat::FormatInvalid, NumberFormat::Unorm, 0), false);
    Expect("  UNORM target with write mask 0", LogicOpAppliesTo(Rgba8, NumberFormat::Unorm, 0),
           false);
    Expect("  float target", LogicOpAppliesTo(DataFormat::Format16_16_16_16, NumberFormat::Float,
                                              0xF),
           false);
    Expect("  sRGB target", LogicOpAppliesTo(Rgba8, NumberFormat::Srgb, 0xF), false);
    Expect("  UNORM target, written", LogicOpAppliesTo(Rgba8, NumberFormat::Unorm, 0xF), true);
    Expect("  UINT target, alpha only", LogicOpAppliesTo(Rgba8, NumberFormat::Uint, 0x8), true);
    std::fprintf(stderr, "logicOp\n");
    Expect("  copy ROP on an UNORM target, no logicOp", LogicOpUnsupported(Op::Copy, true, false),
           false);
    Expect("  xor ROP, no applicable target, no logicOp", LogicOpUnsupported(Op::Xor, false, false),
           false);
    Expect("  xor ROP, applicable target, logicOp present", LogicOpUnsupported(Op::Xor, true, true),
           false);
    Expect("  xor ROP, applicable target, no logicOp", LogicOpUnsupported(Op::Xor, true, false),
           true);

    std::fprintf(stderr, "depthBounds\n");
    Expect("  test disabled", DepthBoundsUnsupported(false, true, 0.2f, 0.8f, false), false);
    Expect("  enabled, no depth buffer", DepthBoundsUnsupported(true, false, 0.2f, 0.8f, false),
           false);
    Expect("  enabled, bounds [0, 1]", DepthBoundsUnsupported(true, true, 0.0f, 1.0f, false), false);
    Expect("  enabled, bounds [-0.5, 1.5]", DepthBoundsUnsupported(true, true, -0.5f, 1.5f, false),
           false);
    Expect("  enabled, bounds [0.2, 0.8], feature present",
           DepthBoundsUnsupported(true, true, 0.2f, 0.8f, true), false);
    Expect("  enabled, bounds [0, 0.99]", DepthBoundsUnsupported(true, true, 0.0f, 0.99f, false),
           true);
    Expect("  enabled, bounds [0.01, 1]", DepthBoundsUnsupported(true, true, 0.01f, 1.0f, false),
           true);

    std::fprintf(stderr, "drawIndirectCount\n");
    Expect("  indirect draw without a count buffer", DrawIndirectCountUnsupported(0, false), false);
    Expect("  count buffer, feature present", DrawIndirectCountUnsupported(0x1000, true), false);
    Expect("  count buffer, no feature", DrawIndirectCountUnsupported(0x1000, false), true);

    std::fprintf(stderr, "rasterized primitive\n");
    const auto raster = [](PrimitiveType prim, bool gs = false, Gs out = Gs::TriangleStrip,
                           bool tess = false,
                           TessellationTopology topo = TessellationTopology::TriangleCw) {
        return RasterizedPrimitive(prim, gs, out, tess, topo);
    };
    const auto lines = [&](RasterPrimitive r, bool poly = false, PolygonMode front = PolygonMode::Fill,
                           PolygonMode back = PolygonMode::Fill, bool cull_front = false,
                           bool cull_back = false) {
        return RasterizesLines(r, VisiblePolygonMode(poly, front, back, cull_front, cull_back));
    };
    Expect("  triangle list", lines(raster(PrimitiveType::TriangleList)), false);
    Expect("  rect list", lines(raster(PrimitiveType::RectList)), false);
    Expect("  line list", lines(raster(PrimitiveType::LineList)), true);
    Expect("  adjacency line strip", lines(raster(PrimitiveType::AdjLineStrip)), true);
    Expect("  line list into a triangle-strip GS",
           lines(raster(PrimitiveType::LineList, true, Gs::TriangleStrip)), false);
    Expect("  triangles into a line-strip GS",
           lines(raster(PrimitiveType::TriangleList, true, Gs::LineStrip)), true);
    Expect("  patches tessellated to triangles",
           lines(raster(PrimitiveType::PatchPrimitive, false, Gs::TriangleStrip, true)), false);
    Expect("  patches tessellated to isolines",
           lines(raster(PrimitiveType::PatchPrimitive, false, Gs::TriangleStrip, true,
                        TessellationTopology::Line)),
           true);
    Expect("  polygon mode off (registers say line)",
           lines(raster(PrimitiveType::TriangleList), false, PolygonMode::Line, PolygonMode::Line),
           false);
    Expect("  front faces in line mode",
           lines(raster(PrimitiveType::TriangleList), true, PolygonMode::Line, PolygonMode::Fill),
           true);
    Expect("  front culled, back faces in line mode",
           lines(raster(PrimitiveType::TriangleList), true, PolygonMode::Fill, PolygonMode::Line,
                 true, false),
           true);
    Expect("  front culled, front faces in line mode",
           lines(raster(PrimitiveType::TriangleList), true, PolygonMode::Line, PolygonMode::Fill,
                 true, false),
           false);
    Expect("  back culled, back faces in line mode",
           lines(raster(PrimitiveType::TriangleList), true, PolygonMode::Fill, PolygonMode::Line,
                 false, true),
           false);
    Expect("  points in line polygon mode",
           lines(raster(PrimitiveType::PointList), true, PolygonMode::Line, PolygonMode::Line),
           false);

    std::fprintf(stderr, "wideLines\n");
    Expect("  no lines, width 4", WideLinesUnsupported(false, 4.0f, false), false);
    Expect("  lines, width 1", WideLinesUnsupported(true, 1.0f, false), false);
    Expect("  lines, width 2, feature present", WideLinesUnsupported(true, 2.0f, true), false);
    Expect("  lines, width 2", WideLinesUnsupported(true, 2.0f, false), true);
    Expect("  lines, width 0.5", WideLinesUnsupported(true, 0.5f, false), true);

    std::fprintf(stderr, "front/back polygon modes\n");
    const auto tris = RasterPrimitive::Triangles;
    Expect("  both fill, polygon mode on",
           PolygonModesDiffer(tris, true, PolygonMode::Fill, PolygonMode::Fill, false, false), false);
    Expect("  differing modes, polygon mode off",
           PolygonModesDiffer(tris, false, PolygonMode::Line, PolygonMode::Fill, false, false),
           false);
    Expect("  differing modes, back faces culled",
           PolygonModesDiffer(tris, true, PolygonMode::Fill, PolygonMode::Line, false, true), false);
    Expect("  differing modes, front faces culled",
           PolygonModesDiffer(tris, true, PolygonMode::Fill, PolygonMode::Line, true, false), false);
    Expect("  differing modes on lines",
           PolygonModesDiffer(RasterPrimitive::Lines, true, PolygonMode::Fill, PolygonMode::Line,
                              false, false),
           false);
    Expect("  differing modes, both faces visible",
           PolygonModesDiffer(tris, true, PolygonMode::Fill, PolygonMode::Line, false, false), true);
    Expect("  visible mode: front culled -> back mode",
           VisiblePolygonMode(true, PolygonMode::Fill, PolygonMode::Line, true, false) ==
               PolygonMode::Line,
           true);
    Expect("  visible mode: polygon mode off -> fill",
           VisiblePolygonMode(false, PolygonMode::Line, PolygonMode::Line, false, false) ==
               PolygonMode::Fill,
           true);

    std::fprintf(stderr, "point polygon mode (MoltenVK emulation is inexact)\n");
    Expect("  triangles filled", PointPolygonModeInexact(tris, PolygonMode::Fill), false);
    Expect("  triangles in line mode", PointPolygonModeInexact(tris, PolygonMode::Line), false);
    Expect("  point list (mode irrelevant)",
           PointPolygonModeInexact(RasterPrimitive::Points, PolygonMode::Point), false);
    Expect("  front culled, visible back faces filled",
           PointPolygonModeInexact(tris, VisiblePolygonMode(true, PolygonMode::Point,
                                                            PolygonMode::Fill, true, false)),
           false);
    Expect("  triangles in point mode", PointPolygonModeInexact(tris, PolygonMode::Point), true);

    std::fprintf(stderr, "shaderStorageImageMultisample\n");
    Expect("  single-sample storage view", StorageMultisampleUnsupported(true, 1, false), false);
    Expect("  4-sample written depth attachment (not storage)",
           StorageMultisampleUnsupported(false, 4, false), false);
    Expect("  4-sample storage view, feature present", StorageMultisampleUnsupported(true, 4, true),
           false);
    Expect("  4-sample storage view, no feature", StorageMultisampleUnsupported(true, 4, false),
           true);

    std::fprintf(stderr, "%s: %u failures\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
