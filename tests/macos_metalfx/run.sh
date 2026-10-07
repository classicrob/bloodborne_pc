#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
[[ $(uname -s) == Darwin ]] || { echo 'MetalFX prototype requires macOS.' >&2; exit 1; }
prefix=${BB_DEPS_PREFIX:-$HOME/bbport-deps/x86_64}
fsr=${BB_FSR_STATIC_DIR:-$PWD/out/gpu/third_party/fsr-vulkan}
mkdir -p out/macos-metalfx
for library in libffx_vulkan_portable.a libffx_vulkan_fsr3_vk_backend_1_1_4.a libffx_vulkan_fsr3_host_1_1_4.a; do
    [[ -f $fsr/$library ]] || { echo "Build the shared GPU tree first; missing $fsr/$library" >&2; exit 1; }
done
clang++ -arch x86_64 -std=c++20 -O2 -fobjc-arc -Wall -Wextra -Werror \
    -Wno-missing-field-initializers -DVK_ENABLE_BETA_EXTENSIONS \
    -I"$prefix/include" -Igpu/third_party/fsr-vulkan/include tests/macos_metalfx/interop.mm \
    "$fsr/libffx_vulkan_portable.a" "$fsr/libffx_vulkan_fsr3_vk_backend_1_1_4.a" \
    "$fsr/libffx_vulkan_fsr3_host_1_1_4.a" -L"$prefix/lib" -lvulkan -lSDL3 \
    -Wl,-rpath,"$prefix/lib" -framework Foundation -framework Metal -framework MetalFX \
    -o out/macos-metalfx/interop
export VK_DRIVER_FILES="$prefix/share/vulkan/icd.d/MoltenVK_icd.json"
[[ -f $VK_DRIVER_FILES ]] || { echo "Missing ICD: $VK_DRIVER_FILES" >&2; exit 1; }
exec /usr/bin/arch -x86_64 out/macos-metalfx/interop "$@"
