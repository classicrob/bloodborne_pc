#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/../.."
deps=${BB_DEPS_PREFIX:-$HOME/bbport-deps/x86_64}
mkdir -p out/mac-external-host
/opt/homebrew/bin/glslangValidator -V --target-env vulkan1.2 tests/macos_external_host/bda.comp -o out/mac-external-host/bda.spv
clang -arch x86_64 -std=c11 -O2 -Wall -Wextra -Werror -I. -Isrc -I"$deps/include" \
    tests/macos_external_host/probe.c -L"$deps/lib" -Wl,-rpath,"$deps/lib" -lvulkan -o out/mac-external-host/probe
export VK_DRIVER_FILES="$deps/share/vulkan/icd.d/MoltenVK_icd.json"
arch -x86_64 out/mac-external-host/probe out/mac-external-host/bda.spv
