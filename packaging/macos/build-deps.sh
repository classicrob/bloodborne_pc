#!/usr/bin/env bash
# Cross-build linked libraries; Homebrew tools and header-only packages run on the host.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
[[ $(uname -s) == Darwin ]] || { echo 'This dependency setup requires macOS.' >&2; exit 1; }
prefix=${BB_DEPS_PREFIX:-$HOME/bbport-deps/x86_64}
mkdir -p "$prefix" out/deps-src out/deps-build
prefix=$(cd "$prefix" && pwd)
scratch=$PWD/out
export PATH="/opt/homebrew/bin:$PATH"
jobs=${BB_BUILD_JOBS:-8}
for tool in cmake ninja pkg-config python3; do
    command -v "$tool" >/dev/null || { echo "Install host tool $tool first." >&2; exit 1; }
done
brew_prefix=${BB_HOST_BREW_PREFIX:-/opt/homebrew}
for package in boost robin-map; do
    [[ -d $brew_prefix/opt/$package ]] || {
        echo "Install host header package: $brew_prefix/bin/brew install $package" >&2; exit 1;
    }
done

fetch_tar() {
    local name=$1 url=$2 archive=$3 directory=$4
    if [[ ! -d $scratch/deps-src/$directory ]]; then
        [[ -f $scratch/deps-src/$archive ]] || curl -fLsS "$url" -o "$scratch/deps-src/$archive"
        tar -xf "$scratch/deps-src/$archive" -C "$scratch/deps-src"
    fi
}
build_cmake() {
    local name=$1 source=$2 artifact=$3
    shift 3
    [[ ! -f $prefix/$artifact ]] || return 0
    cmake -S "$scratch/deps-src/$source" -B "$scratch/deps-build/$name" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=x86_64 \
        -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_C_COMPILER=/usr/bin/clang \
        -DCMAKE_CXX_COMPILER=/usr/bin/clang++ "$@"
    cmake --build "$scratch/deps-build/$name" --parallel "$jobs"
    cmake --install "$scratch/deps-build/$name"
}

if [[ ! -f $prefix/lib/libMoltenVK.dylib || ! -f $prefix/include/vma/vk_mem_alloc.h ]]; then
    sdk_version=1.4.363.0
    sdk=$scratch/deps-build/vulkan-sdk
    installer=$scratch/deps-build/vulkan-installer
    if [[ ! -f $sdk/macOS/lib/libMoltenVK.dylib ]]; then
        curl -fLsS "https://sdk.lunarg.com/sdk/download/$sdk_version/mac/vulkan_sdk.zip" \
            -o "$scratch/deps-src/vulkan-sdk.zip"
        unzip -q -o "$scratch/deps-src/vulkan-sdk.zip" -d "$installer"
        "$installer/vulkansdk-macOS-$sdk_version.app/Contents/MacOS/vulkansdk-macOS-$sdk_version" \
            --root "$sdk" --accept-licenses --default-answer --confirm-command \
            install com.lunarg.vulkan.core com.lunarg.vulkan.vma copy_only=1
    fi
    python3 - "$sdk/macOS" "$prefix" "$installer" "$scratch/deps-src/vulkan-sdk.zip" <<'PY'
from pathlib import Path
import shutil
import sys
sdk, prefix, installer, archive = map(Path, sys.argv[1:])
for directory in ('lib/pkgconfig', 'bin', 'include', 'share/vulkan/icd.d'):
    (prefix / directory).mkdir(parents=True, exist_ok=True)
loader = next((sdk / 'lib').glob('libvulkan.1.*.dylib'))
for source in (loader, sdk / 'lib/libMoltenVK.dylib'):
    shutil.copy2(source, prefix / 'lib' / source.name)
for name in ('libvulkan.dylib', 'libvulkan.1.dylib'):
    target = prefix / 'lib' / name
    if target.is_symlink():
        target.unlink()
    target.symlink_to(loader.name)
for name in ('vulkan', 'vk_video', 'vma'):
    shutil.copytree(sdk / 'include' / name, prefix / 'include' / name, dirs_exist_ok=True)
shutil.copy2(sdk / 'bin/vulkaninfo', prefix / 'bin/vulkaninfo')
shutil.copy2(sdk / 'share/vulkan/icd.d/MoltenVK_icd.json', prefix / 'share/vulkan/icd.d/MoltenVK_icd.json')
# Keep the SDK's ARM-only comparison driver, but never select it for an x86_64 build.
for source in sdk.glob('lib/*kosmic*'):
    shutil.copy2(source, prefix / 'lib' / source.name)
for source in sdk.glob('share/vulkan/icd.d/*kosmic*'):
    shutil.copy2(source, prefix / 'share/vulkan/icd.d' / source.name)
pc = (sdk / 'lib/pkgconfig/vulkan.pc').read_text()
pc = '\n'.join('prefix=' + str(prefix) if line.startswith('prefix=') else line for line in pc.splitlines()) + '\n'
(prefix / 'lib/pkgconfig/vulkan.pc').write_text(pc)
shutil.rmtree(sdk.parent)
shutil.rmtree(installer)
archive.unlink()
PY
fi

# Copy only CMake header metadata; target includes share the existing host headers.
python3 - "$prefix" "$brew_prefix" <<'PY'
from pathlib import Path
import shutil
import sys
prefix, brew = map(Path, sys.argv[1:])
(prefix / 'include').mkdir(parents=True, exist_ok=True)
def copy_metadata(source, destination):
    if destination.exists():
        for path in destination.rglob('*'):
            if path.is_file():
                path.chmod(path.stat().st_mode | 0o200)
    shutil.copytree(source, destination, dirs_exist_ok=True, copy_function=shutil.copyfile)

for name, package in (('boost', 'boost'), ('tsl', 'robin-map')):
    target = prefix / 'include' / name
    if not target.exists():
        target.symlink_to(brew / 'opt' / package / 'include' / name, target_is_directory=True)
for source in (brew / 'opt/boost/lib/cmake').iterdir():
    if source.is_dir() and source.name.startswith(('Boost-', 'boost_headers-')):
        copy_metadata(source, prefix / 'lib/cmake' / source.name)
for source in (brew / 'opt/robin-map/share/cmake').iterdir():
    copy_metadata(source, prefix / 'share/cmake' / source.name)
PY

fetch_tar fmt https://github.com/fmtlib/fmt/archive/refs/tags/11.2.0.tar.gz fmt.tar.gz fmt-11.2.0
build_cmake fmt fmt-11.2.0 lib/libfmt.a -DFMT_TEST=OFF -DFMT_DOC=OFF
fetch_tar sdl https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz sdl.tar.gz SDL3-3.4.18
build_cmake sdl SDL3-3.4.18 lib/libSDL3.dylib -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF \
    -DSDL_STATIC=OFF -DSDL_X11=OFF -DSDL_FRAMEWORK=OFF -DSDL_INSTALL=ON
fetch_tar xxhash https://github.com/Cyan4973/xxHash/archive/refs/tags/v0.8.3.tar.gz xxhash.tar.gz xxHash-0.8.3
build_cmake xxhash xxHash-0.8.3/cmake_unofficial lib/libxxhash.a -DBUILD_SHARED_LIBS=OFF -DXXHASH_BUILD_XXHSUM=OFF
if [[ ! -d $scratch/deps-src/zydis ]]; then
    git clone --depth 1 --branch v4.1.1 --recurse-submodules --shallow-submodules \
        https://github.com/zyantific/zydis.git "$scratch/deps-src/zydis"
fi
build_cmake zycore zydis/dependencies/zycore lib/libZycore.a -DZYCORE_BUILD_TESTS=OFF -DZYCORE_BUILD_EXAMPLES=OFF
build_cmake zydis zydis lib/libZydis.a -DZYDIS_BUILD_TOOLS=OFF -DZYDIS_BUILD_EXAMPLES=OFF -DZYDIS_BUILD_DOXYGEN=OFF

fetch_tar ffmpeg https://ffmpeg.org/releases/ffmpeg-7.1.1.tar.xz ffmpeg.tar.xz ffmpeg-7.1.1
if [[ ! -f $prefix/lib/pkgconfig/libavformat.pc ]]; then
    mkdir -p "$scratch/deps-build/ffmpeg"
    (
        cd "$scratch/deps-build/ffmpeg"
        "$scratch/deps-src/ffmpeg-7.1.1/configure" --prefix="$prefix" --arch=x86_64 \
            --target-os=darwin --enable-cross-compile --cc=/usr/bin/clang \
            --extra-cflags='-arch x86_64' --extra-ldflags='-arch x86_64' --install-name-dir=@rpath \
            --disable-autodetect --disable-asm --disable-programs --disable-doc --disable-debug \
            --disable-static --enable-shared --disable-avdevice --disable-avfilter --disable-network \
            --disable-everything --enable-decoder=h264,hevc,aac --enable-demuxer=mov,mpegts \
            --enable-parser=h264,hevc,aac --enable-protocol=file
        make -j"$jobs"
        make install
    )
fi
for library in libSDL3.dylib libfmt.a libxxhash.a libZydis.a libZycore.a libavcodec.dylib libavformat.dylib libavutil.dylib libswscale.dylib libswresample.dylib libvulkan.dylib libMoltenVK.dylib; do
    lipo "$prefix/lib/$library" -verify_arch x86_64
done
echo "Dependencies ready: export BB_DEPS_PREFIX=$prefix"
