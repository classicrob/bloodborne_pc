#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
mkdir -p out/macos_spike
xcrun clang -arch x86_64 -std=c11 -O2 -Wall -Wextra -Werror -I. -Isrc \
    tests/macos_spike/test_tls_runtime.c src/compat_mac.c \
    -Wl,-dead_strip -o out/macos_spike/test_tls_runtime
exec arch -x86_64 out/macos_spike/test_tls_runtime
