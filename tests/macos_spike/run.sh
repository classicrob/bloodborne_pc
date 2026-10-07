#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
mkdir -p out/macos_spike
xcrun clang -arch x86_64 -std=c11 -O2 -Wall -Wextra -Werror -pthread \
    tests/macos_spike/spike.c -o out/macos_spike/spike
exec arch -x86_64 out/macos_spike/spike "$@"
