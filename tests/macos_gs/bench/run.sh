#!/usr/bin/env bash
# Opus-GS design measurement: cost of a render-pass boundary on MoltenVK (tests/macos_gs/bench).
set -euo pipefail
cd "$(dirname "$0")/../../.."
: "${BB_DEPS_PREFIX:?Set BB_DEPS_PREFIX to the x86_64 dependency prefix}"
out="${BB_GS_BENCH_OUT:-out/macos_gs/bench}"
mkdir -p "$out"
for f in tri.vert mrt.frag touch.comp; do
    glslangValidator -V --target-env vulkan1.2 -o "$out/$f.spv" "tests/macos_gs/bench/$f" >/dev/null
done
clang -arch x86_64 -std=c11 -O2 -Wall -Wextra -Werror -I"$BB_DEPS_PREFIX/include" \
    tests/macos_gs/bench/pass_break.c -L"$BB_DEPS_PREFIX/lib" -lvulkan \
    -Wl,-rpath,"$BB_DEPS_PREFIX/lib" -o "$out/pass_break"
python3 packaging/macos/sign.py "$out/pass_break"
for full in 0 1; do
    for size in "1920 1080" "2560 1440" "3840 2160"; do
        arch -x86_64 env VK_DRIVER_FILES="$BB_DEPS_PREFIX/share/vulkan/icd.d/MoltenVK_icd.json" \
            "$out/pass_break" "$out" $size $full
    done
done
