#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
: "${BB_GPU_BUILD:?Set BB_GPU_BUILD to a stable GPU library directory}"
prefix=${BB_DEPS_PREFIX:-$HOME/bbport-deps/x86_64}
output=${BB_PKRTZ_TEST_OUT:-out/mac-pkrtz}
mkdir -p "$output/rte"
cxx() {
    clang++ -arch x86_64 -std=c++23 -O2 -UNDEBUG -fPIC -fno-omit-frame-pointer \
        -Wno-deprecated-declarations -Wno-missing-field-initializers \
        -include gpu/shim/bb_mac_compat.h \
        -Igpu/shim -Igpu/shadps4 -Igpu/third_party/gcn/include \
        -Igpu/third_party/sirit/include -Igpu/third_party/sirit/externals/SPIRV-Headers/include \
        -I"$prefix/include" -I/opt/homebrew/include "$@" \
        tests/test_gpu_runtime_stubs.cpp -Wl,-export_dynamic \
        -L"$BB_GPU_BUILD" -Wl,-rpath,"$BB_GPU_BUILD" -lbbgpu \
        -L"$prefix/lib" -Wl,-rpath,"$prefix/lib" -lfmt
}
cxx tests/macos_pkrtz/emit.cpp \
    gpu/shadps4/shader_recompiler/backend/spirv/emit_spirv_bitwise_conversion.cpp \
    -o "$output/emit"
cxx tests/macos_pkrtz/optimizer.cpp \
    gpu/shadps4/shader_recompiler/frontend/translate/vector_alu.cpp \
    gpu/shadps4/shader_recompiler/ir/ir_emitter.cpp \
    gpu/shadps4/shader_recompiler/ir/passes/constant_propagation_pass.cpp \
    -o "$output/optimizer"
clang -arch x86_64 -std=c11 -O2 -Wall -Wextra -Werror -DBB_PKRTZ_CASES \
    -I"$prefix/include" tests/macos_float_modes/probe.c \
    -L"$prefix/lib" -Wl,-rpath,"$prefix/lib" -lvulkan -o "$output/probe"
python3 packaging/macos/sign.py "$output/emit" "$output/optimizer" "$output/probe"
arch -x86_64 "$output/optimizer"
arch -x86_64 "$output/emit" "$output"
export VK_DRIVER_FILES="$prefix/share/vulkan/icd.d/MoltenVK_icd.json"
for mode in 0 1 2 3; do
    spirv-val --target-env vulkan1.2 "$output/mode-$mode.spv"
    arch -x86_64 "$output/probe" "$output/mode-$mode.spv" "$mode" \
        > "$output/mode-$mode.log" 2>&1
    cat "$output/mode-$mode.log"
done
arch -x86_64 "$output/emit" "$output/rte" --rte
spirv-val --target-env vulkan1.2 "$output/rte/mode-3.spv"
status=0
arch -x86_64 "$output/probe" "$output/rte/mode-3.spv" 3 \
    > "$output/rte-control.log" 2>&1 || status=$?
test "$status" -eq 2
tail -1 "$output/rte-control.log"
