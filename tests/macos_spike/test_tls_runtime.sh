#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
mkdir -p out/macos_spike
xcrun clang -arch x86_64 -std=c11 -O2 -Wall -Wextra -Werror -I. -Isrc \
    tests/macos_spike/test_tls_runtime.c src/compat_mac.c \
    -Wl,-dead_strip -o out/macos_spike/test_tls_runtime
arch -x86_64 out/macos_spike/test_tls_runtime
xcrun clang -arch x86_64 -std=c11 -O2 -Wall -Wextra -Werror -DBB_TLS_REAL_MEMORY -I. -Isrc \
    tests/macos_spike/test_tls_runtime.c src/runtime_memory.c src/compat_mac.c \
    -Wl,-dead_strip -o out/macos_spike/test_tls_real_memory
exec arch -x86_64 out/macos_spike/test_tls_real_memory
