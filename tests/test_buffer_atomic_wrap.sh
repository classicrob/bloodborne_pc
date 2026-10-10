#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
: "${BB_GPU_BUILD:?Set BB_GPU_BUILD to a stable shared out/gpu directory}"
prefix=${BB_DEPS_PREFIX:-$HOME/bbport-deps/x86_64}
mkdir -p out/mac-atomic-wrap
clang++ -arch x86_64 -std=c++23 -O2 -c tests/test_gpu_runtime_stubs.cpp -o out/mac-atomic-wrap/stubs.o
clang++ -arch x86_64 -std=c++23 -O2 -UNDEBUG -fno-omit-frame-pointer \
    -fvisibility=hidden -fvisibility-inlines-hidden \
    -Wno-deprecated-declarations -Wno-missing-field-initializers \
    -include gpu/shim/bb_mac_compat.h \
    -Igpu/shim -Igpu/shadps4 -Igpu/third_party/gcn/include \
    -Igpu/third_party/sirit/include -Igpu/third_party/sirit/externals/SPIRV-Headers/include \
    -I"$prefix/include" -I/opt/homebrew/include \
    tests/test_buffer_atomic_wrap.cpp \
    gpu/shadps4/shader_recompiler/backend/spirv/emit_spirv_atomic.cpp \
    out/mac-atomic-wrap/stubs.o -Wl,-export_dynamic \
    -L"$BB_GPU_BUILD" -Wl,-rpath,"$BB_GPU_BUILD" -lbbgpu \
    -L"$prefix/lib" -Wl,-rpath,"$prefix/lib" -lfmt \
    -o out/mac-atomic-wrap/test
python3 packaging/macos/sign.py out/mac-atomic-wrap/test
export VK_DRIVER_FILES="$prefix/share/vulkan/icd.d/MoltenVK_icd.json"
arch -x86_64 out/mac-atomic-wrap/test out/mac-atomic-wrap
for name in inc dec; do
    spirv-val --target-env vulkan1.2 "out/mac-atomic-wrap/$name.spv"
done
