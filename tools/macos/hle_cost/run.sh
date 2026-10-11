#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
: "${BB_HLE_COST_GPU:?Set BB_HLE_COST_GPU to a stable private libbbgpu.dylib}"
: "${BB_HLE_COST_ATRAC:?Set BB_HLE_COST_ATRAC to libatrac9.a}"
: "${BB_HLE_COST_INCLUDE_ROOT:=$PWD}"
deps=/Users/roberthaisfield/bbport-deps/x86_64
export PKG_CONFIG_LIBDIR="$deps/lib/pkgconfig:$deps/share/pkgconfig"
export PKG_CONFIG_PATH="$PKG_CONFIG_LIBDIR"
read -r -a includes <<< "$(pkg-config --cflags vulkan sdl3)"
read -r -a libs <<< "$(pkg-config --libs vulkan sdl3)"
mkdir -p out/hle-cost
capture_dir=$(mktemp -d "$PWD/out/hle-cost/run.XXXXXX")
cp "$BB_HLE_COST_GPU" "$capture_dir/libbbgpu.dylib"
shasum -a 256 "$capture_dir/libbbgpu.dylib" > "$capture_dir/gpu.sha256"
cp tools/macos/hle_cost/probe.c tools/macos/hle_cost/run.sh "$capture_dir/"
git rev-parse HEAD > "$capture_dir/commit.txt"
git diff -- src > "$capture_dir/runtime.diff"
compile=(clang -arch x86_64 -std=c11 -O2 -g -Wall -Wextra -Werror -pthread "${includes[@]}" -I. -Isrc -I"$BB_HLE_COST_INCLUDE_ROOT" \
  tools/macos/hle_cost/probe.c src/runtime*.c src/compat_mac.c src/macos_ui.c \
  "$BB_HLE_COST_ATRAC" -lm -L"$capture_dir" -lbbgpu -Wl,-rpath,@loader_path \
  -Wl,-rpath,"$deps/lib" -Wl,-export_dynamic "${libs[@]}" -o "$capture_dir/probe")
clang --version > "$capture_dir/compiler.txt"
printf '%q ' "${compile[@]}" > "$capture_dir/compile-command.sh"
printf '\n' >> "$capture_dir/compile-command.sh"
"${compile[@]}"
python3 packaging/macos/sign.py "$capture_dir/probe"
uptime > "$capture_dir/host-before.txt"
env -u BB_HLE_TRACE -u BB_HLE_TRACE_SUMMARY -u BB_CPU_PROFILE arch -x86_64 "$capture_dir/probe" > "$capture_dir/raw.log" 2> "$capture_dir/stderr.log"
sed -n '/^case,round,/,$p' "$capture_dir/raw.log" > "$capture_dir/samples.csv"
uptime > "$capture_dir/host-after.txt"
printf 'Evidence: %s\n' "$capture_dir"
