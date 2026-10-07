#!/bin/sh
# Reuse a completed shared GPU build; compile only this test and the repaired emitter.
set -eu
cd "$(dirname "$0")/../.."
: "${BB_GPU_BUILD:?Set BB_GPU_BUILD to the completed shared out/gpu directory}"
prefix=${BB_DEPS_PREFIX:-$HOME/bbport-deps/x86_64}
mkdir -p out/mac-float-modes
clang++ -arch x86_64 -std=c++23 -O2 -UNDEBUG -fno-omit-frame-pointer \
    -Wno-deprecated-declarations -Wno-missing-field-initializers \
    -include gpu/shim/bb_mac_compat.h \
    -Igpu/shim -Igpu/shadps4 -Igpu/third_party/gcn/include \
    -Igpu/third_party/sirit/include -Igpu/third_party/sirit/externals/SPIRV-Headers/include \
    -I"$prefix/include" -I/opt/homebrew/include \
    tests/macos_float_modes/emit.cpp \
    gpu/shadps4/shader_recompiler/backend/spirv/emit_spirv_floating_point.cpp \
    tests/test_gpu_runtime_stubs.cpp \
    -L"$BB_GPU_BUILD" -Wl,-rpath,"$BB_GPU_BUILD" -lbbgpu \
    -L"$prefix/lib" -Wl,-rpath,"$prefix/lib" -lfmt \
    -o out/mac-float-modes/emit
codesign --force --sign - out/mac-float-modes/emit
codesign --verify --strict out/mac-float-modes/emit
arch -x86_64 out/mac-float-modes/emit out/mac-float-modes
for name in baseline production baseline-bench production-bench; do
    spirv-val --target-env vulkan1.2 "out/mac-float-modes/$name.spv"
    spirv-dis "out/mac-float-modes/$name.spv" -o "out/mac-float-modes/$name.spvasm"
done
