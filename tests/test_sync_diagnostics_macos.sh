#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p out/mac-sync
clang -arch x86_64 -O2 -g -std=c11 -Wall -Wextra -Werror -Isrc \
    tests/test_sync_diagnostics_macos.c src/runtime_mutex.c src/runtime_rwlock.c src/runtime_hle_trace.c \
    src/runtime_memory.c src/compat_mac.c -o out/mac-sync/test-sync-diagnostics
python3 packaging/macos/sign.py out/mac-sync/test-sync-diagnostics
exec arch -x86_64 out/mac-sync/test-sync-diagnostics
