#!/usr/bin/env bash
# Geometry emulation (Apple) tools: builds compile_roles and conformance against this checkout's shader recompiler
# (compiled into the executable, production flags) and the shared GPU library for everything else.
#
#   BB_GPU_BUILD=<tree>/out/gpu BB_GPU_THIRD_PARTY=<tree>/gpu/third_party tests/macos_gs/build.sh
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
root=$(cd -- "$here/../.." && pwd)
deps=${BB_DEPS_PREFIX:-$HOME/bbport-deps/x86_64}
gpu_build=${BB_GPU_BUILD:-$root/out/gpu}
third=${BB_GPU_THIRD_PARTY:-$root/gpu/third_party}
out=${BB_GS_TEST_OUT:-$root/out/macos_gs}
[[ -f $gpu_build/libbbgpu.dylib ]] || { echo "No libbbgpu.dylib in $gpu_build (set BB_GPU_BUILD)" >&2; exit 1; }
obj="$out/obj"
mkdir -p "$obj"
cxx=(/usr/bin/clang++ -arch x86_64 -std=gnu++2b -fPIC -fno-omit-frame-pointer
     -Wno-deprecated-declarations -Wno-missing-field-initializers
     -DBOOST_ASIO_STANDALONE -DMINIZ_STATIC_DEFINE -DZYCORE_STATIC_BUILD -DZYDIS_STATIC_BUILD
     -include "$root/gpu/shim/bb_mac_compat.h"
     -I"$root/gpu/shim" -I"$root/gpu/shadps4" -I"$gpu_build/include"
     -I"$third/gcn/include" -I"$third/half/include" -I"$third/xbyak"
     -I"$third/magic_enum/include" -I"$third/magic_enum/include/magic_enum"
     -I"$third/sirit/include" -I"$third/sirit/externals/SPIRV-Headers/include"
     -I"$deps/include/vma" -isystem "$deps/include")
srcs=()
while IFS= read -r f; do srcs+=("$f"); done < <(find "$root/gpu/shadps4/shader_recompiler" -name '*.cpp' | sort)
objs=()
pids=()
for f in "${srcs[@]}"; do
    o="$obj/$(echo "${f#$root/}" | tr / _).o"
    objs+=("$o")
    key="$(printf '%s\n' "${cxx[@]}" | shasum -a 256 | cut -c1-16) $(cat "$f" $(find "$root/gpu/shadps4/shader_recompiler" "$root/gpu/shadps4/video_core/amdgpu" -name '*.h') | shasum -a 256 | cut -c1-64)"
    if [[ -f $o && -f $o.key && "$(cat "$o.key")" == "$key" ]]; then continue; fi
    rm -f "$o" "$o.key"
    ( "${cxx[@]}" -O2 -DNDEBUG -c "$f" -o "$o" && echo "$key" > "$o.key" ) &
    pids+=($!)
    if (( ${#pids[@]} >= ${BB_BUILD_JOBS:-6} )); then
        wait "${pids[0]}" || { echo "compile failed" >&2; exit 1; }
        pids=("${pids[@]:1}")
    fi
done
for p in "${pids[@]}"; do wait "$p" || { echo "compile failed" >&2; exit 1; }; done
"${cxx[@]}" -I"$root" -O1 -g -Wall -Wno-unused-parameter -c "$here/compile_roles.cpp" -o "$obj/compile_roles.o"
"${cxx[@]}" -I"$root" -O1 -g -Wall -Wno-unused-parameter -c "$here/conformance.cpp" -o "$obj/conformance.o"
for f in flat.frag list.vert; do
    glslangValidator -V --target-env vulkan1.3 -o "$out/$f.spv" "$here/$f" >/dev/null
done
# The production setup kernel of indirect ES/GS draws and the point compaction kernel.
glslangValidator -V --target-env vulkan1.3 -o "$out/gs_indirect_setup.comp.spv" \
    "$root/gpu/shadps4/video_core/host_shaders/gs_indirect_setup.comp" >/dev/null
glslangValidator -V --target-env vulkan1.3 -o "$out/gs_point_compact.comp.spv" \
    "$root/gpu/shadps4/video_core/host_shaders/gs_point_compact.comp" >/dev/null
"${cxx[@]}" -O1 -c "$root/tests/test_gpu_runtime_stubs.cpp" -o "$obj/stubs.o"
/usr/bin/clang++ -arch x86_64 -Wl,-export_dynamic "$obj/compile_roles.o" "$obj/stubs.o" "${objs[@]}" \
    "$gpu_build/libbbgpu.dylib" -L"$deps/lib" -lfmt \
    -Wl,-rpath,"$gpu_build" -Wl,-rpath,"$deps/lib" -o "$out/compile_roles"
/usr/bin/clang++ -arch x86_64 -Wl,-export_dynamic "$obj/conformance.o" "$obj/stubs.o" "${objs[@]}" \
    "$gpu_build/libbbgpu.dylib" -L"$deps/lib" -lfmt -lvulkan \
    -Wl,-rpath,"$gpu_build" -Wl,-rpath,"$deps/lib" -o "$out/conformance"
for tool in compile_roles conformance; do
    python3 "$root/packaging/macos/sign.py" "$out/$tool" >/dev/null
    echo "$out/$tool"
done
