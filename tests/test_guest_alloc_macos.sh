#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p out/mac-memory
clang -arch x86_64 -std=c11 -Wall -Wextra -Werror -Isrc \
    -c src/runtime_memory.c -o out/mac-memory/runtime_memory.o
clang++ -arch x86_64 -std=c++20 -Wall -Wextra -Werror -Igpu/shim \
    tests/test_guest_alloc_macos.cpp out/mac-memory/runtime_memory.o \
    -o out/mac-memory/test-guest-alloc
arch -x86_64 out/mac-memory/test-guest-alloc
