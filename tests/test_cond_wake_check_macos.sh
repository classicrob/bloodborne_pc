#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p out/cond-wake
clang -arch x86_64 -O2 -g -std=c11 -fno-omit-frame-pointer -Wall -Wextra -Werror -Isrc \
    tests/test_cond_wake_check_macos.c src/runtime_memory.c src/compat_mac.c -o out/cond-wake/test
python3 packaging/macos/sign.py out/cond-wake/test
arch -x86_64 out/cond-wake/test off
BB_COND_WAKE_CHECK=1 arch -x86_64 out/cond-wake/test on
exec env BB_COND_WAKE_CHECK=1 BB_HLE_TRACE=scePthreadCondWait arch -x86_64 out/cond-wake/test traced
