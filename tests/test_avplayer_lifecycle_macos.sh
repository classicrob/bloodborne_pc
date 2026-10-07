#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
prefix=${BB_DEPS_PREFIX:-$HOME/bbport-deps/x86_64}
mkdir -p out/mac-avplayer
# Native fixture generation; no proprietary asset and no second GPU build.
ffmpeg -hide_banner -loglevel error -y -f lavfi -i 'color=c=blue:s=64x64:r=30:d=1' \
    -f lavfi -i 'sine=frequency=440:sample_rate=48000:duration=1' \
    -c:v libx264 -preset ultrafast -tune zerolatency -c:a aac -shortest out/mac-avplayer/fixture.mp4
for unit in runtime_thread runtime_memory compat_mac; do
    xcrun clang -arch x86_64 -std=c11 -O2 -Wall -Wextra -Werror -Isrc \
        -c "src/$unit.c" -o "out/mac-avplayer/$unit.o"
done
xcrun clang++ -arch x86_64 -std=c++23 -O1 -g -UNDEBUG \
    -Wno-deprecated-declarations -include gpu/shim/bb_mac_compat.h \
    -Igpu/shim -Igpu/shadps4 -Igpu/third_party/magic_enum/include \
    -I"$prefix/include" -I/opt/homebrew/include \
    tests/test_avplayer_lifecycle_macos.cpp \
    gpu/shadps4/core/libraries/avplayer/avplayer_source.cpp \
    gpu/shadps4/core/libraries/avplayer/avplayer_file_streamer.cpp \
    gpu/shadps4/core/libraries/videodec/video_utils.cpp gpu/shim/common/assert.cpp \
    out/mac-avplayer/runtime_thread.o out/mac-avplayer/runtime_memory.o out/mac-avplayer/compat_mac.o \
    -L"$prefix/lib" -Wl,-rpath,"$prefix/lib" -Wl,-dead_strip \
    -lavformat -lavcodec -lavutil -lswresample -lswscale -lfmt \
    -o out/mac-avplayer/test-lifecycle
python3 packaging/macos/sign.py out/mac-avplayer/test-lifecycle
exec arch -x86_64 out/mac-avplayer/test-lifecycle out/mac-avplayer/fixture.mp4
