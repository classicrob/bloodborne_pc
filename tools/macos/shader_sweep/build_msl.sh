#!/usr/bin/env bash
# Builds out/tools/shader-msl (msl.cpp, the sweep's MSL stage) natively against SPIRV-Cross at the
# revision MoltenVK 1.4.2 vendors (its ExternalRevisions/SPIRV-Cross_repo_revision). Fetches the
# source once into out/tools/src/spirv-cross.
#   tools/macos/shader_sweep/build_msl.sh
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
root=$(cd -- "$here/../../.." && pwd)
revision=6c09849fe88c48eaed08413aa022aaa136a3a057
tools=$root/out/tools
src=$tools/src/spirv-cross
build=$tools/build/spirv-cross
if [[ "$(git -C "$src" rev-parse HEAD 2>/dev/null)" != "$revision" ]]; then
    mkdir -p "$src"
    git -C "$src" init -q
    git -C "$src" fetch -q --depth 1 https://github.com/KhronosGroup/SPIRV-Cross.git "$revision"
    git -C "$src" checkout -q FETCH_HEAD
fi
cmake -S "$src" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DSPIRV_CROSS_ENABLE_TESTS=OFF \
    -DSPIRV_CROSS_CLI=OFF > /dev/null
ninja -C "$build" spirv-cross-msl spirv-cross-glsl spirv-cross-core > /dev/null
clang++ -std=c++17 -O2 -I"$src" "$here/msl.cpp" "$build/libspirv-cross-msl.a" \
    "$build/libspirv-cross-glsl.a" "$build/libspirv-cross-core.a" -o "$tools/shader-msl"
echo "$tools/shader-msl"
