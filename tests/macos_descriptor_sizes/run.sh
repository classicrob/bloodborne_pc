#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
: "${BB_DEPS_PREFIX:?Set BB_DEPS_PREFIX to the x86_64 dependency prefix}"
out_dir="${BB_DESCRIPTOR_TEST_OUT:-out/macos_descriptor_sizes}"
mkdir -p "$out_dir"
glslangValidator -V --target-env vulkan1.2 tests/macos_descriptor_sizes/sizes.comp -o "$out_dir/sizes.spv"
spirv-val --target-env vulkan1.2 "$out_dir/sizes.spv"
clang -arch x86_64 -std=c11 -O2 -Wall -Wextra -Werror \
    -I"$BB_DEPS_PREFIX/include" tests/macos_descriptor_sizes/probe.c \
    -L"$BB_DEPS_PREFIX/lib" -lvulkan -Wl,-rpath,"$BB_DEPS_PREFIX/lib" \
    -o "$out_dir/probe"
python3 packaging/macos/sign.py "$out_dir/probe"
arch -x86_64 env VK_DRIVER_FILES="$BB_DEPS_PREFIX/share/vulkan/icd.d/MoltenVK_icd.json" \
    "$out_dir/probe" "$out_dir/sizes.spv" "$@"
