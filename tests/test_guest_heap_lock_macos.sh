#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p out/mac-heap-lock
clang -arch x86_64 -O2 -g -std=c11 -Wall -Wextra -Werror -I. -Isrc \
    tests/test_guest_heap_lock_macos.c src/runtime_sema.c src/runtime_thread.c \
    src/runtime_memory.c src/compat_mac.c -Wl,-dead_strip -o out/mac-heap-lock/test-heap-lock
python3 packaging/macos/sign.py out/mac-heap-lock/test-heap-lock
exec arch -x86_64 out/mac-heap-lock/test-heap-lock
