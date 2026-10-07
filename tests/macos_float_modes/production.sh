#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
prefix=${BB_DEPS_PREFIX:-$HOME/bbport-deps/x86_64}
sh tests/macos_float_modes/build_emitter.sh
clang -arch x86_64 -std=c11 -O2 -Wall -Wextra -Werror -I"$prefix/include" \
    tests/macos_float_modes/probe.c -L"$prefix/lib" -Wl,-rpath,"$prefix/lib" \
    -lvulkan -o out/mac-float-modes/probe
codesign --force --sign - out/mac-float-modes/probe
codesign --verify --strict out/mac-float-modes/probe
export VK_DRIVER_FILES="$prefix/share/vulkan/icd.d/MoltenVK_icd.json"
export MVK_CONFIG_FAST_MATH_ENABLED=2
status=0
arch -x86_64 out/mac-float-modes/probe out/mac-float-modes/baseline.spv \
    > out/mac-float-modes/production-baseline.log 2>&1 || status=$?
# The unmodified strict-multiply emitter must reproduce the counterexample.
[ "$status" = 2 ] || { cat out/mac-float-modes/production-baseline.log; exit 1; }
arch -x86_64 out/mac-float-modes/probe out/mac-float-modes/production.spv \
    > out/mac-float-modes/production.log 2>&1
cat out/mac-float-modes/production.log
if [ "${BB_FLOAT_BENCHMARK:-0}" = 1 ]; then
    repetition=0
    for variant in baseline production production baseline baseline production; do
        arch -x86_64 out/mac-float-modes/probe "out/mac-float-modes/$variant-bench.spv" --benchmark \
            > "out/mac-float-modes/bench-$repetition.log" 2>&1
        repetition=$((repetition+1))
    done
fi
python3 tests/macos_float_modes/verify.py
