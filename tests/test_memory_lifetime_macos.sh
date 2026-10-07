#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p out/mac-memory
clang -arch x86_64 -std=c11 -Wall -Wextra -Werror -Isrc \
    tests/test_memory_lifetime_macos.c -o out/mac-memory/test-lifetime
python3 packaging/macos/sign.py out/mac-memory/test-lifetime
exec arch -x86_64 out/mac-memory/test-lifetime
