#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p out/memory-clamp
if [ "$(uname -s)" = Darwin ]; then
    clang -arch x86_64 -std=c11 -Wall -Wextra -Werror -Isrc \
        tests/test_memory_clamp.c -o out/memory-clamp/test
    python3 packaging/macos/sign.py out/memory-clamp/test
    exec arch -x86_64 out/memory-clamp/test
fi
cc -std=c11 -Wall -Wextra -Werror -Isrc tests/test_memory_clamp.c \
    -pthread -o out/memory-clamp/test
exec out/memory-clamp/test
