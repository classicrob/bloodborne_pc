// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace AmdGpu {
struct ComputeProgram;
union Regs;
} // namespace AmdGpu

namespace Shader {
struct Info;
}

namespace Vulkan {

class Rasterizer;

/// Attempts to execute a shader using HLE if possible.
bool ExecuteShaderHLE(const Shader::Info& info, const AmdGpu::Regs& regs,
                      const AmdGpu::ComputeProgram& cs_program, Rasterizer& rasterizer);

#ifdef __APPLE__
/// After a dispatch is recorded: BB_RT_FILL_VERIFY=2 checks the bytes of a fill kernel that
/// ExecuteShaderHLE left to run as the reference.
void AfterDispatchHLE(Rasterizer& rasterizer);
#endif

} // namespace Vulkan
