#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p out/movie-handoff
xcrun clang -arch x86_64 -std=c11 -O2 -g -fno-omit-frame-pointer -Wall -Wextra -Werror \
    -I. -Isrc tests/test_movie_handoff_macos.c -o out/movie-handoff/test
python3 packaging/macos/sign.py out/movie-handoff/test
arch -x86_64 out/movie-handoff/test untraced
arch -x86_64 out/movie-handoff/test traced
exec arch -x86_64 out/movie-handoff/test summary
