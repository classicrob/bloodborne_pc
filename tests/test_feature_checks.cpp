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

    std::fprintf(stderr, "logicOp\n");
    Expect("  copy ROP, colour target, no logicOp", LogicOpUnsupported(Op::Copy, 1, false), false);
    Expect("  xor ROP, no colour target, no logicOp", LogicOpUnsupported(Op::Xor, 0, false), false);
    Expect("  xor ROP, colour target, logicOp present", LogicOpUnsupported(Op::Xor, 1, true), false);
    Expect("  xor ROP, colour target, no logicOp", LogicOpUnsupported(Op::Xor, 1, false), true);

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

    std::fprintf(stderr, "wideLines\n");
    Expect("  triangle list, width 4", WideLinesUnsupported(PrimitiveType::TriangleList,
                                                           PolygonMode::Fill, 4.0f, false),
           false);
    Expect("  rect list, width 0.125", WideLinesUnsupported(PrimitiveType::RectList,
                                                           PolygonMode::Fill, 0.125f, false),
           false);
    Expect("  point polygon mode, width 2", WideLinesUnsupported(PrimitiveType::TriangleList,
                                                                PolygonMode::Point, 2.0f, false),
           false);
    Expect("  line list, width 1", WideLinesUnsupported(PrimitiveType::LineList, PolygonMode::Fill,
                                                       1.0f, false),
           false);
    Expect("  line strip, width 2, feature present",
           WideLinesUnsupported(PrimitiveType::LineStrip, PolygonMode::Fill, 2.0f, true), false);
    Expect("  line list, width 2", WideLinesUnsupported(PrimitiveType::LineList, PolygonMode::Fill,
                                                       2.0f, false),
           true);
    Expect("  line polygon mode, width 2", WideLinesUnsupported(PrimitiveType::TriangleList,
                                                               PolygonMode::Line, 2.0f, false),
           true);
    Expect("  adjacency line strip, width 0.5",
           WideLinesUnsupported(PrimitiveType::AdjLineStrip, PolygonMode::Fill, 0.5f, false), true);

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
