#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
mkdir -p out/macos_spike
for unit in runtime_thread runtime_memory compat_mac; do
    xcrun clang -arch x86_64 -std=c11 -O2 -Wall -Wextra -Werror -Isrc \
        -c "src/$unit.c" -o "out/macos_spike/hle-$unit.o"
done
xcrun clang++ -arch x86_64 -std=c++20 -O2 -Wall -Wextra -Werror \
    -Igpu/shim -Igpu/shadps4 tests/macos_spike/test_hle_stack.cpp \
    out/macos_spike/hle-runtime_thread.o out/macos_spike/hle-runtime_memory.o \
    out/macos_spike/hle-compat_mac.o -Wl,-dead_strip -o out/macos_spike/test_hle_stack
python3 packaging/macos/sign.py out/macos_spike/test_hle_stack
exec arch -x86_64 out/macos_spike/test_hle_stack
