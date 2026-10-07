#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p out/mac-memory
clang -arch x86_64 -std=c11 -Wall -Wextra -Werror -Isrc \
    tests/test_memory_macos.c src/runtime_memory.c -o out/mac-memory/test-memory
arch -x86_64 out/mac-memory/test-memory
