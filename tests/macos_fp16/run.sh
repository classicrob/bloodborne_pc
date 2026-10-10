#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
: "${BB_GPU_BUILD:?Set BB_GPU_BUILD to a stable GPU library directory}"
prefix=${BB_DEPS_PREFIX:-$HOME/bbport-deps/x86_64}
output=${BB_FP16_TEST_OUT:-out/mac-fp16}
mkdir -p "$output"
clang++ -arch x86_64 -std=c++23 -O2 -UNDEBUG -fno-omit-frame-pointer \
    -Wno-deprecated-declarations -Wno-missing-field-initializers \
    -include gpu/shim/bb_mac_compat.h \
    -Igpu/shim -Igpu/shadps4 -Igpu/third_party/gcn/include \
    -Igpu/third_party/sirit/include -Igpu/third_party/sirit/externals/SPIRV-Headers/include \
    -I"$prefix/include" -I/opt/homebrew/include \
    tests/macos_fp16/emit.cpp \
    gpu/shadps4/shader_recompiler/backend/spirv/emit_spirv_convert.cpp \
    tests/test_gpu_runtime_stubs.cpp -Wl,-export_dynamic \
    -L"$BB_GPU_BUILD" -Wl,-rpath,"$BB_GPU_BUILD" -lbbgpu \
    -L"$prefix/lib" -Wl,-rpath,"$prefix/lib" -lfmt -o "$output/emit"
clang++ -arch x86_64 -std=c++23 -O2 -UNDEBUG -fno-omit-frame-pointer \
    -Wno-deprecated-declarations -Wno-missing-field-initializers \
    -include gpu/shim/bb_mac_compat.h \
    -Igpu/shim -Igpu/shadps4 -Igpu/third_party/gcn/include \
    -Igpu/third_party/sirit/include -Igpu/third_party/sirit/externals/SPIRV-Headers/include \
    -Igpu/third_party/magic_enum/include -I"$prefix/include" -I/opt/homebrew/include \
    tests/macos_fp16/entry.cpp \
    gpu/shadps4/shader_recompiler/backend/spirv/emit_spirv.cpp \
    gpu/shadps4/shader_recompiler/backend/spirv/emit_spirv_convert.cpp \
    gpu/shadps4/shader_recompiler/backend/spirv/emit_spirv_context_get_set.cpp \
    gpu/shadps4/shader_recompiler/backend/spirv/emit_spirv_shared_memory.cpp \
    tests/test_gpu_runtime_stubs.cpp -Wl,-export_dynamic \
    -L"$BB_GPU_BUILD" -Wl,-rpath,"$BB_GPU_BUILD" -lbbgpu \
    -L"$prefix/lib" -Wl,-rpath,"$prefix/lib" -lfmt -o "$output/entry"
clang -arch x86_64 -std=c11 -O2 -Wall -Wextra -Werror -DBB_FP16_CASES \
    -I"$prefix/include" tests/macos_float_modes/probe.c \
    -L"$prefix/lib" -Wl,-rpath,"$prefix/lib" -lvulkan -o "$output/probe"
python3 packaging/macos/sign.py "$output/emit" "$output/entry" "$output/probe"
arch -x86_64 "$output/emit" "$output"
export VK_DRIVER_FILES="$prefix/share/vulkan/icd.d/MoltenVK_icd.json"
status=0
for mode in 0 1 2 3; do
    spirv-val --target-env vulkan1.2 "$output/mode-$mode.spv"
    spirv-dis "$output/mode-$mode.spv" -o "$output/mode-$mode.spvasm"
    arch -x86_64 "$output/probe" "$output/mode-$mode.spv" "$mode" \
        > "$output/mode-$mode.log" 2>&1 || status=2
    cat "$output/mode-$mode.log"
done
python3 tests/macos_fp16/policy.py "$output"
exit "$status"
