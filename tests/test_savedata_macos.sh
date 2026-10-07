#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p out/mac-playability
clang -arch x86_64 -std=c11 -Wall -Wextra -Werror -I. -Isrc \
    tests/test_savedata_macos.c -o out/mac-playability/test-savedata
arch -x86_64 out/mac-playability/test-savedata
python3 -m unittest discover -s tests -p test_mods.py
