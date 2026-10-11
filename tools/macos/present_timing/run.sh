#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")/../../.."
prefix=${BB_DEPS_PREFIX:-$HOME/bbport-deps/x86_64}
mkdir -p out/flip-audit
clang++ --version
shasum -a 256 "$prefix/lib/libMoltenVK.dylib"
set -x
clang++ -arch x86_64 -std=c++20 -O2 -Wall -Wextra -Werror -Wno-missing-field-initializers \
    -I"$prefix/include" tools/macos/present_timing/probe.cpp \
    -L"$prefix/lib" -lvulkan -lSDL3 -Wl,-rpath,"$prefix/lib" -o out/flip-audit/present-timing
python3 packaging/macos/sign.py out/flip-audit/present-timing
export VK_DRIVER_FILES="$prefix/share/vulkan/icd.d/MoltenVK_icd.json"
exec arch -x86_64 out/flip-audit/present-timing
