#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
: "${BB_DEPS_PREFIX:?Set BB_DEPS_PREFIX to the x86_64 dependency prefix}"
out_dir="${BB_FP64_TEST_OUT:-out/macos_fp64_policy}"
mkdir -p "$out_dir"
ir=gpu/shadps4/shader_recompiler/ir
clang++ -arch x86_64 -std=c++2b -O2 -UNDEBUG \
    -Igpu/shim -Igpu/shadps4 -Igpu/third_party/magic_enum/include \
    -Igpu/third_party/gcn/include -Igpu/third_party/half/include -I"$BB_DEPS_PREFIX/include" \
    tests/test_fp64_policy.cpp gpu/shim/common/assert.cpp gpu/shim/common/logging/log.cpp \
    "$ir/basic_block.cpp" "$ir/ir_emitter.cpp" "$ir/microinstruction.cpp" \
    "$ir/opcodes.cpp" "$ir/value.cpp" "$ir/type.cpp" \
    "$ir/passes/lower_fp64_to_fp32.cpp" \
    -L"$BB_DEPS_PREFIX/lib" -lfmt -Wl,-dead_strip -Wl,-rpath,"$BB_DEPS_PREFIX/lib" \
    -o "$out_dir/probe"
python3 packaging/macos/sign.py "$out_dir/probe"
arch -x86_64 "$out_dir/probe"
