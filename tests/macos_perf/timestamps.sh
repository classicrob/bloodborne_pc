#!/usr/bin/env bash
# MoltenVK timestamp qualification for BB_PERF (tests/macos_perf/timestamps.c).
set -euo pipefail
cd "$(dirname "$0")/../.."
: "${BB_DEPS_PREFIX:?Set BB_DEPS_PREFIX to the x86_64 dependency prefix}"
out_dir="${BB_PERF_TEST_OUT:-out/macos_perf}"
mkdir -p "$out_dir"
glslangValidator -V --target-env vulkan1.2 tests/macos_perf/busy.comp -o "$out_dir/busy.spv"
spirv-val --target-env vulkan1.2 "$out_dir/busy.spv"
clang -arch x86_64 -std=c11 -O2 -Wall -Wextra -Werror \
    -I"$BB_DEPS_PREFIX/include" tests/macos_perf/timestamps.c \
    -L"$BB_DEPS_PREFIX/lib" -lvulkan -lm -Wl,-rpath,"$BB_DEPS_PREFIX/lib" \
    -o "$out_dir/timestamps"
python3 packaging/macos/sign.py "$out_dir/timestamps"
arch -x86_64 env VK_DRIVER_FILES="$BB_DEPS_PREFIX/share/vulkan/icd.d/MoltenVK_icd.json" \
    "$out_dir/timestamps" "$out_dir/busy.spv"
