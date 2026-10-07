#!/usr/bin/env bash
# Builds the GCN2 buffer conformance suite (x86_64, runs under Rosetta on Apple Silicon).
#
#   tests/gcn_buffer_conformance/build.sh [--tree DIR] [--out DIR]
#
# Links the shared GPU library (BB_GPU_BUILD, default: this checkout's out/gpu) for everything
# outside the shader recompiler. --tree DIR names the source tree under test: its headers are
# used, and its gpu/shadps4/shader_recompiler/**/*.cpp and gpu/third_party/sirit/src are compiled
# into the executable (with the same flags as the GPU build), so a branch's recompiler is tested
# without a second GPU build. Without --tree, the recompiler inside the dylib is tested and the
# headers come from this checkout (they must match the dylib's sources). To test a commit rather
# than a working tree:  git archive <commit> gpu/shadps4 gpu/shim gpu/third_party/sirit src/compat_mac.h | tar -x -C DIR
# and pass --tree DIR with BB_TREE_NAME=<commit>.
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
root=$(cd -- "$here/../.." && pwd)
tree=""
out="$root/out/gcnconf"
while [[ $# -gt 0 ]]; do
    case $1 in
    --tree) tree=$(cd -- "$2" && pwd); shift 2 ;;
    --out) out=$2; shift 2 ;;
    *) echo "usage: $0 [--tree DIR] [--out DIR]" >&2; exit 2 ;;
    esac
done
deps=${BB_DEPS_PREFIX:-$HOME/bbport-deps/x86_64}
gpu_build=${BB_GPU_BUILD:-$root/out/gpu}
third=${BB_GPU_THIRD_PARTY:-$root/gpu/third_party}
[[ -f $gpu_build/libbbgpu.dylib ]] || { echo "No libbbgpu.dylib in $gpu_build (set BB_GPU_BUILD)" >&2; exit 1; }
[[ -d $third/magic_enum/include ]] || { echo "No third-party headers in $third (set BB_GPU_THIRD_PARTY)" >&2; exit 1; }
src=${tree:-$root}
if [[ -n ${BB_TREE_NAME:-} ]]; then
    name=$BB_TREE_NAME
elif [[ -n $tree ]]; then
    name="tree-$(git -C "$tree" rev-parse --short HEAD)$(git -C "$tree" diff --quiet HEAD -- gpu || echo -dirty)"
else
    name=dylib
fi
obj="$out/obj-$name"
mkdir -p "$obj"

cxx=(/usr/bin/clang++ -arch x86_64 -std=gnu++2b -fPIC -fno-omit-frame-pointer
     -Wno-deprecated-declarations -Wno-missing-field-initializers
     -DBOOST_ASIO_STANDALONE -DMINIZ_STATIC_DEFINE -DZYCORE_STATIC_BUILD -DZYDIS_STATIC_BUILD
     -include "$src/gpu/shim/bb_mac_compat.h"
     -I"$src/gpu/shim" -I"$src/gpu/shadps4" -I"$gpu_build/include"
     -I"$third/gcn/include" -I"$third/half/include" -I"$third/xbyak"
     -I"$third/magic_enum/include" -I"$third/magic_enum/include/magic_enum"
     -I"$src/gpu/third_party/sirit/include" -I"$third/sirit/externals/SPIRV-Headers/include"
     -I"$deps/include/vma" -isystem "$deps/include")
# Production flags for recompiler objects (CMake RelWithDebInfo), our own asserts kept on in the harness.
prod=(-O2 -DNDEBUG)

extra=()
if [[ -n $tree ]]; then
    srcs=()
    while IFS= read -r f; do srcs+=("$f"); done < <(find "$tree/gpu/shadps4/shader_recompiler" -name '*.cpp' | sort)
    while IFS= read -r f; do srcs+=("$f"); done < <(find "$tree/gpu/third_party/sirit/src" -name '*.cpp' | sort)
    echo "compiling ${#srcs[@]} recompiler/sirit sources from $tree ($name)"
    pids=()
    for f in "${srcs[@]}"; do
        o="$obj/$(echo "${f#$tree/}" | tr / _).o"
        extra+=("$o")
        if [[ -f $o && $o -nt $f ]]; then continue; fi
        "${cxx[@]}" "${prod[@]}" -I"$tree/gpu/third_party/sirit/src" -c "$f" -o "$o" &
        pids+=($!)
        if (( ${#pids[@]} >= ${BB_BUILD_JOBS:-8} )); then
            wait "${pids[0]}" || { echo "compile failed" >&2; exit 1; }
            pids=("${pids[@]:1}")
        fi
    done
    for p in "${pids[@]}"; do wait "$p" || { echo "compile failed" >&2; exit 1; }; done
fi

/usr/bin/clang -arch x86_64 -std=c11 -O2 -Wall -Wextra -Werror -c "$here/ref.c" -o "$obj/ref.o"
"${cxx[@]}" -O1 -g -UNDEBUG -Wall -Wno-unused-parameter -c "$here/harness.cpp" -o "$obj/harness.o"
"${cxx[@]}" -O1 -c "$root/tests/test_gpu_runtime_stubs.cpp" -o "$obj/stubs.o"
/usr/bin/clang++ -arch x86_64 -Wl,-export_dynamic "$obj/harness.o" "$obj/ref.o" "$obj/stubs.o" "${extra[@]}" \
    "$gpu_build/libbbgpu.dylib" -L"$deps/lib" -lfmt \
    -Wl,-rpath,"$gpu_build" -Wl,-rpath,"$deps/lib" -o "$out/gcn-conformance-$name"
echo "$out/gcn-conformance-$name"
