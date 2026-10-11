#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p out/hle-trace
xcrun clang -arch x86_64 -std=c11 -mavx -O2 -g -fno-omit-frame-pointer -Wall -Wextra -Werror \
    -I. -Isrc tests/test_hle_trace_macos.c -o out/hle-trace/test
python3 packaging/macos/sign.py out/hle-trace/test
arch -x86_64 out/hle-trace/test
exec arch -x86_64 out/hle-trace/test summary
