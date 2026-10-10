#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
prefix=${BB_DEPS_PREFIX:-$HOME/bbport-deps/x86_64}
fsr=${BB_FSR_STATIC_DIR:-$PWD/out/gpu/third_party/fsr-vulkan}
mkdir -p out/macos-metalfx
glslang --target-env vulkan1.3 -V tests/macos_metalfx/jitter.comp -o out/macos-metalfx/jitter.spv
spirv-val --target-env vulkan1.3 out/macos-metalfx/jitter.spv
clang++ -arch x86_64 -std=c++20 -O2 -fobjc-arc -Wall -Wextra -Werror \
    -Wno-missing-field-initializers -DVK_ENABLE_BETA_EXTENSIONS \
    -I"$prefix/include" -Igpu/third_party/fsr-vulkan/include tests/macos_metalfx/jitter.mm \
    gpu/shim/macos_metalfx.mm "$fsr/libffx_vulkan_portable.a" \
    "$fsr/libffx_vulkan_fsr3_vk_backend_1_1_4.a" "$fsr/libffx_vulkan_fsr3_host_1_1_4.a" \
    -L"$prefix/lib" -lvulkan -lSDL3 -Wl,-rpath,"$prefix/lib" \
    -framework Foundation -framework Metal -framework MetalFX -framework Security \
    -o out/macos-metalfx/jitter
python3 packaging/macos/sign.py out/macos-metalfx/jitter
export VK_DRIVER_FILES="$prefix/share/vulkan/icd.d/MoltenVK_icd.json"
exec arch -x86_64 out/macos-metalfx/jitter out/macos-metalfx/jitter.spv
