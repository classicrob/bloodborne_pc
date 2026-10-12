#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p out/mac-playability
clang -arch x86_64 -std=c11 -O2 -g -Wall -Wextra -Werror -pthread \
    tests/test_player_position_macos.c -o out/mac-playability/test-player-position
python3 packaging/macos/sign.py out/mac-playability/test-player-position
arch -x86_64 out/mac-playability/test-player-position "$@"
