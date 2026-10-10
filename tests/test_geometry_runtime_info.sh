#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
prefix=${BB_DEPS_PREFIX:-$HOME/bbport-deps/x86_64}
mkdir -p out/geometry-runtime-info
"${CXX:-clang++}" -std=c++23 -O2 -Wall -Wextra -Werror \
    -Wno-unused-parameter -Wno-missing-field-initializers \
    -Wno-unused-variable -Wno-reorder-ctor -UNDEBUG -DFMT_HEADER_ONLY \
    -Igpu/shim -Igpu/shadps4 -Igpu/third_party/gcn/include \
    -Igpu/third_party/magic_enum/include -I"$prefix/include" -I/opt/homebrew/include \
    tests/test_geometry_runtime_info.cpp -o out/geometry-runtime-info/test
out/geometry-runtime-info/test
