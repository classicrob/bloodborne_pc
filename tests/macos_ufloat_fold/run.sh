#!/bin/sh
# The 10_11_11 inverse-fold control (optimizer.cpp) against a tree's IR emitter and constant
# propagation pass: BB_TREE (default this checkout), linked with BB_GPU_BUILD's library.
set -eu
cd "$(dirname "$0")/../.."
: "${BB_GPU_BUILD:?Set BB_GPU_BUILD to a stable GPU library directory}"
tree=${BB_TREE:-.}
prefix=${BB_DEPS_PREFIX:-$HOME/bbport-deps/x86_64}
output=${BB_UFLOAT_FOLD_OUT:-out/mac-ufloat-fold}
mkdir -p "$output"
clang++ -arch x86_64 -std=c++23 -O2 -UNDEBUG -fPIC -fno-omit-frame-pointer \
    -Wno-deprecated-declarations -Wno-missing-field-initializers \
    -include "$tree/gpu/shim/bb_mac_compat.h" \
    -I"$tree/gpu/shim" -I"$tree/gpu/shadps4" -Igpu/third_party/gcn/include \
    -I"$tree/gpu/third_party/sirit/include" -Igpu/third_party/sirit/externals/SPIRV-Headers/include \
    -Igpu/third_party/magic_enum/include -I"$prefix/include" -I/opt/homebrew/include \
    tests/macos_ufloat_fold/optimizer.cpp \
    "$tree/gpu/shadps4/shader_recompiler/ir/ir_emitter.cpp" \
    "$tree/gpu/shadps4/shader_recompiler/ir/passes/constant_propagation_pass.cpp" \
    tests/test_gpu_runtime_stubs.cpp -Wl,-export_dynamic \
    -L"$BB_GPU_BUILD" -Wl,-rpath,"$BB_GPU_BUILD" -lbbgpu \
    -L"$prefix/lib" -Wl,-rpath,"$prefix/lib" -lfmt \
    -o "$output/optimizer"
python3 packaging/macos/sign.py "$output/optimizer"
exec arch -x86_64 "$output/optimizer"
