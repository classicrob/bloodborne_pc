#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
prefix=${BB_DEPS_PREFIX:-$HOME/bbport-deps/x86_64}
mkdir -p out/mac-float-modes
/opt/homebrew/bin/glslangValidator -V --target-env vulkan1.2 \
    tests/macos_float_modes/float.comp -o out/mac-float-modes/float.spv
clang -arch x86_64 -std=c11 -O2 -Wall -Wextra -Werror -I"$prefix/include" \
    tests/macos_float_modes/probe.c -L"$prefix/lib" -Wl,-rpath,"$prefix/lib" \
    -lvulkan -o out/mac-float-modes/probe
export VK_DRIVER_FILES="$prefix/share/vulkan/icd.d/MoltenVK_icd.json"
python3 tests/macos_float_modes/variants.py
result=0
for variant in default strict preserve allow-contraction; do
    shader=float
    fast=2
    case "$variant" in
        strict) fast=0 ;;
        preserve) shader=preserve ;;
        allow-contraction) shader=allow-contraction; fast=0 ;;
    esac
    # Every run is sequential. Exit 2 records an observed semantic failure.
    status=0
    MVK_CONFIG_FAST_MATH_ENABLED=$fast arch -x86_64 out/mac-float-modes/probe \
        "out/mac-float-modes/$shader.spv" > "out/mac-float-modes/$variant.log" 2>&1 || status=$?
    cat "out/mac-float-modes/$variant.log"
    if [ "$status" != 0 ] && [ "$status" != 2 ]; then exit "$status"; fi
    if [ "$status" = 2 ]; then result=2; fi
done
exit "$result"
