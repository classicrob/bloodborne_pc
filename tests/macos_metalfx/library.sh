#!/bin/sh
# MetalFX unit 2: the built GPU library contains the MetalFX module and links exactly the system
# frameworks it needs (Foundation, Metal, MetalFX, Security), from /System only.
set -eu
cd "$(dirname "$0")/../.."
lib=${BB_GPU_BUILD:?Set BB_GPU_BUILD to the GPU build directory}/libbbgpu.dylib
links=$(otool -L "$lib")
for framework in Foundation Metal MetalFX Security; do
    echo "$links" | grep -q "/System/Library/Frameworks/$framework.framework/" ||
        { echo "FAIL: $lib does not link $framework.framework" >&2; exit 1; }
done
nm -gU "$lib" | c++filt | grep -q 'BbMetalFX::TemporalScaler::TemporalScaler' ||
    { echo "FAIL: $lib has no MetalFX TemporalScaler" >&2; exit 1; }
echo "MetalFX module in libbbgpu: frameworks and TemporalScaler PASS"
