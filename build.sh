#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")"
# macOS: the whole build (and --test) runs in a machine-wide heavy-job slot (tools/macos/heavy.sh;
# BB_HEAVY_PRIORITY=1 is the critical lane: shared builds).
if [[ $(uname -s) == Darwin && -z ${BB_HEAVY_HELD:-} ]]; then
    case ${BB_HEAVY_PRIORITY:-0} in
    1) heavy_priority=(--priority) ;;
    0) heavy_priority=() ;;
    *) echo 'BB_HEAVY_PRIORITY must be 0 or 1.' >&2; exit 1 ;;
    esac
    exec tools/macos/heavy.sh ${heavy_priority[@]+"${heavy_priority[@]}"} "build.sh $* in $(basename "$(dirname "$PWD")")/$(basename "$PWD")" \
        -- bash "$PWD/build.sh" "$@"
fi
mkdir -p out
platform_flags=()
probe_link_flags=(-no-pie)
compat=()
cmake_platform=()
ninja_jobs=()
default_lto=ON
sign_darwin() {
    if [[ $(uname -s) == Darwin ]]; then
        python3 packaging/macos/sign.py "$@"
    fi
}
case $(uname -s) in
Darwin)
    # Guest code executes natively, so all linked libraries must contain x86_64.
    deps=${BB_DEPS_PREFIX:-$HOME/bbport-deps/x86_64}
    export PATH="$deps/bin:/opt/homebrew/bin:$PATH"
    export PKG_CONFIG_LIBDIR="$deps/lib/pkgconfig:$deps/share/pkgconfig"
    export PKG_CONFIG_PATH="$PKG_CONFIG_LIBDIR"
    export VK_DRIVER_FILES="$deps/share/vulkan/icd.d/MoltenVK_icd.json"
    [[ -f $VK_DRIVER_FILES ]] || {
        echo "Missing macOS MoltenVK ICD: $VK_DRIVER_FILES (run packaging/macos/build-deps.sh)." >&2
        exit 1
    }
    export CMAKE_PREFIX_PATH="$deps"
    platform_flags=(-arch x86_64)
    probe_link_flags=(-Wl,-no_pie)
    compat=(src/compat_mac.c src/macos_ui.c)
    cmake_platform=(-DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_IGNORE_PREFIX_PATH=/opt/homebrew
                    -DCMAKE_C_COMPILER="${CC:-/usr/bin/clang}" -DCMAKE_CXX_COMPILER="${CXX:-/usr/bin/clang++}")
    default_lto=OFF
    # Reproducible output: ld64 and ar record input modification times (the debug map's N_OSO
    # entries, archive members) unless ZERO_AR_DATE is set. With it, two clean builds of one
    # commit give byte-identical libbbgpu.dylib and bb-probe.
    export ZERO_AR_DATE=1
    ninja_jobs=(-j "${BB_BUILD_JOBS:-8}")
    # No compiler cache: one cached object differed from a clean compile and clean builds are not
    # yet bit-reproducible, so a cache cannot be shown equivalent to clean. The launchers are
    # cleared on every configure, so a CMake cache from an experiment cannot keep one.
    if [[ ${BB_CCACHE:-0} != 0 ]]; then
        echo 'BB_CCACHE: compiler caching is disabled until builds are shown bit-reproducible.' >&2
        exit 1
    fi
    cmake_platform+=(-DCMAKE_C_COMPILER_LAUNCHER= -DCMAKE_CXX_COMPILER_LAUNCHER=
                     -DCMAKE_OBJC_COMPILER_LAUNCHER= -DCMAKE_OBJCXX_COMPILER_LAUNCHER=)
    ;;
esac
# Windows: MSYS2 CLANG64 (packages in packaging/windows/README.md). The GPU library is static
# there and CMake links bb-probe.exe as well; no PGO (GCC-only flags), LTO off by default.
case $(uname -s) in
MINGW*|MSYS*)
    # shellcheck source=msys2-env.sh
    source ./msys2-env.sh
    for tool in clang cmake ninja pkg-config; do
        command -v "$tool" >/dev/null || { echo "Need $tool from MSYS2 CLANG64 (packaging/windows/README.md)." >&2; exit 1; }
    done
    if [[ ! -f gpu/third_party/fsr-vulkan/CMakeLists.txt || ! -f gpu/third_party/imgui/imgui.h ||
          ! -f gpu/third_party/xbyak/xbyak/xbyak.h ]]; then
        git submodule update --init --recursive
    fi
    for patch in gpu/patches/fsr-vulkan/*.patch; do
        if ! git -C gpu/third_party/fsr-vulkan apply --reverse --check "$PWD/$patch" 2>/dev/null; then
            git -C gpu/third_party/fsr-vulkan apply "$PWD/$patch"
        fi
    done
    cmake -S gpu -B out/gpu -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBB_PGO=off \
        -DBB_LTO="${BB_LTO:-OFF}" -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ >/dev/null
    echo "GPU library and loader: LTO ${BB_LTO:-OFF}"
    if ! ninja -C out/gpu bb-probe bb-gpu-capabilities bb-play > out/gpu-build.log 2>&1; then
        grep -v '^\[' out/gpu-build.log | tail -40 >&2
        echo 'Build failed (full log: out/gpu-build.log)' >&2; exit 1
    fi
    cp out/gpu/bb-probe.exe out/gpu/bb-gpu-capabilities.exe out/gpu/bb-play.exe out/
    echo "Built $PWD/out/bb-probe.exe"
    exit 0
    ;;
esac
if [[ -z ${CC:-} ]]; then
    CC=$(command -v cc || command -v gcc || true)
    if [[ -z $CC ]]; then
        for candidate in /nix/store/*-gcc-wrapper-*/bin/gcc; do
            if [[ -x $candidate ]]; then CC=$candidate; break; fi
        done
    fi
fi
if [[ -z ${CC:-} ]]; then echo 'Install GCC/Clang or set CC.' >&2; exit 1; fi
# Dependencies come from pkg-config (Vulkan loader/headers, SDL3). On NixOS the
# environment is provided by shell.nix; re-enter it automatically if needed.
if ! { command -v pkg-config >/dev/null && pkg-config --exists vulkan sdl3 && command -v cmake >/dev/null && command -v ninja >/dev/null; }; then
    if [[ -z ${BB_IN_NIX_SHELL:-} ]] && command -v nix-shell >/dev/null; then
        exec env BB_IN_NIX_SHELL=1 nix-shell shell.nix --run "bash build.sh $*"
    fi
    echo 'Need pkg-config with vulkan and sdl3, cmake and ninja (Darwin: set BB_DEPS_PREFIX to the x86_64 dependency prefix; Linux: shell.nix).' >&2; exit 1
fi
read -r -a includes <<< "$(pkg-config --cflags vulkan sdl3)"
read -r -a libraries <<< "$(pkg-config --libs vulkan sdl3)"
# GPU library (shadPS4 video core + drivers), built by CMake into out/gpu/libbbgpu.so.
# BB_PGO: generate (instrumented build that writes pgo/ while the game runs), use, off.
# Default: use the profile in pgo/ when there is one. BB_LTO=OFF disables link-time optimization.
pgo=${BB_PGO:-}
if [[ -z $pgo ]]; then
    if [[ $(uname -s) == Darwin ]]; then pgo=off
    elif [[ -n $(find pgo -name '*.gcda' -print -quit 2>/dev/null) ]]; then pgo=use; else pgo=off; fi
fi
mkdir -p pgo
if [[ $(uname -s) == Darwin && $pgo != off ]]; then
    echo 'macOS PGO is not implemented; BB_PGO must be off.' >&2; exit 1
fi
# Submodules (git clone --recursive, or: git submodule update --init) and this port's changes
# to FSR-Vulkan (gpu/patches/fsr-vulkan), applied to its working tree once.
if [[ ! -f gpu/third_party/fsr-vulkan/CMakeLists.txt || ! -f gpu/third_party/imgui/imgui.h ]]; then
    git submodule update --init --recursive
fi
for patch in gpu/patches/fsr-vulkan/*.patch; do
    if ! git -C gpu/third_party/fsr-vulkan apply --reverse --check "$PWD/$patch" 2>/dev/null; then
        git -C gpu/third_party/fsr-vulkan apply "$PWD/$patch"
    fi
done
cmake -S gpu -B out/gpu -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBB_PGO="$pgo" \
    -DBB_LTO="${BB_LTO:-$default_lto}" -DBB_PGO_DIR="$PWD/pgo" "${cmake_platform[@]}" >/dev/null
echo "GPU library: PGO $pgo, LTO ${BB_LTO:-$default_lto}"
if [[ $(uname -s) == Darwin ]]; then
    if ninja -C out/gpu -t commands bbgpu | grep -q ccache; then
        echo 'STOP: out/gpu compiles through ccache; configure did not clear the launcher.' >&2
        exit 1
    fi
    echo 'GPU library: ccache=off'
fi
# A failed GPU build must stop here: an older libbbgpu.so would otherwise be used silently.
if ! ninja -C out/gpu "${ninja_jobs[@]}" bbgpu > out/gpu-build.log 2>&1; then
    grep -v '^\[' out/gpu-build.log | tail -40 >&2
    echo 'GPU library build failed (full log: out/gpu-build.log)' >&2; exit 1
fi
sign_darwin out/gpu/libbbgpu.dylib
# $ORIGIN/gpu: packaged copies keep the library next to the binary without patching it.
gpu=(-Lout/gpu -lbbgpu -Wl,-rpath,'$ORIGIN/gpu' -Wl,-rpath,"$PWD/out/gpu" -rdynamic)
if [[ $(uname -s) == Darwin ]]; then
    gpu=(-Lout/gpu -lbbgpu -Wl,-rpath,@loader_path/gpu -Wl,-rpath,@loader_path
         -Wl,-rpath,"$deps/lib" -Wl,-export_dynamic)
fi
runtime=(src/runtime*.c "${compat[@]}")
# Third-party decoders: compiled once, without this project's -Werror policy.
atrac9=(third_party/LibAtrac9/C/src/*.c)
if [[ ! -f out/libatrac9.a || -n $(find third_party/LibAtrac9/C/src -newer out/libatrac9.a -name '*.c') ]]; then
    rm -rf out/atrac9 && mkdir -p out/atrac9
    for source in "${atrac9[@]}"; do "$CC" "${platform_flags[@]}" -std=c99 -O2 -g -w -c "$source" -o "out/atrac9/$(basename "${source%.c}").o"; done
    ar rcs out/libatrac9.a out/atrac9/*.o
fi
if [[ $(uname -s) == Darwin ]]; then
    # Objects at stable paths: the debug map names each object, so compiling and linking in one
    # step (random temporary object names) made every bb-probe build differ.
    # The debug map points at out/probe-obj; an old one-step build's bb-probe.dSYM would be stale.
    rm -rf out/probe-obj out/bb-probe.dSYM && mkdir -p out/probe-obj
    probe_objects=()
    for source in src/probe.c "${runtime[@]}" src/vulkan_smoke.c; do
        object=out/probe-obj/$(basename "${source%.c}").o
        "$CC" "${platform_flags[@]}" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread "${includes[@]}" -I. -Isrc \
            -c "$source" -o "$object"
        probe_objects+=("$object")
    done
    "$CC" "${platform_flags[@]}" -g -pthread "${probe_link_flags[@]}" "${probe_objects[@]}" out/libatrac9.a -lm \
        "${gpu[@]}" "${libraries[@]}" -o out/bb-probe
else
"$CC" "${platform_flags[@]}" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread "${probe_link_flags[@]}" "${includes[@]}" -I. -Isrc src/probe.c "${runtime[@]}" src/vulkan_smoke.c out/libatrac9.a -lm "${gpu[@]}" "${libraries[@]}" -o out/bb-probe
fi
sign_darwin out/bb-probe
echo "Built $PWD/out/bb-probe"
if [[ $(uname -s) == Darwin ]]; then
    python3 packaging/macos/check-exports.py out/gpu/libbbgpu.dylib out/bb-probe
fi
# GPU check for run.sh (live_resolution=auto): links only the Vulkan loader.
caps_link=()
if [[ $(uname -s) == Darwin ]]; then
    caps_link=(-Wl,-rpath,@loader_path -Wl,-rpath,"$deps/lib")
fi
"$CC" "${platform_flags[@]}" -std=c11 -O2 -Wall -Wextra -Werror tools/gpu_capabilities.c "${includes[@]}" "${libraries[@]}" "${caps_link[@]}" -o out/bb-gpu-capabilities
sign_darwin out/bb-gpu-capabilities
if [[ ${1:-} == --test || ${1:-} == --test-dry ]]; then
    # --test-dry builds, links and signs everything --test would run, and runs the static checks
    # (export gate, test wiring), but executes no test: t checks each test binary exists and is
    # signed, and each test script exists. Run it before merging a change to build.sh or a test
    # target. Tests that compile inside their own scripts are only checked to exist.
    dry=''
    if [[ $1 == --test-dry ]]; then dry=1; fi
    t() {
        if [[ -z $dry ]]; then "$@"; return; fi
        local program=$1
        case $program in
        sh|bash|python3) program=$2 ;;
        spirv-val) return 0 ;;
        esac
        if [[ $program == */* ]]; then
            [[ -e $program ]] || { echo "test-dry: $program does not exist" >&2; exit 1; }
            if [[ $program == *out/* && $(uname -s) == Darwin ]]; then
                codesign -v "$program" 2>/dev/null || { echo "test-dry: $program is not signed" >&2; exit 1; }
            fi
        fi
        echo "test-dry: would run $*"
    }
    if [[ $(uname -s) == Darwin ]]; then
        # The GPU library has no default user directory on macOS: fixtures get a scratch one.
        export BB_GPU_USER_DIR="$PWD/out/gpu-test-user"
        ninja -C out/gpu "${ninja_jobs[@]}" null-descriptor-test upscaler-support-test buffer-bounds-test buffer-dead-resources-test host-import-arena-test vertex-pulling-pipeline-test shader-permutation-info-test gpu-user-dir-test push-userdata-test macos-perf-test depth-bounds-shader-test shader-inputs-test flatten-base-shape-test
        sign_darwin out/gpu/null-descriptor-test out/gpu/upscaler-support-test out/gpu/buffer-bounds-test out/gpu/buffer-dead-resources-test out/gpu/host-import-arena-test out/gpu/vertex-pulling-pipeline-test out/gpu/shader-permutation-info-test out/gpu/gpu-user-dir-test out/gpu/push-userdata-test out/gpu/macos-perf-test out/gpu/depth-bounds-shader-test out/gpu/shader-inputs-test out/gpu/flatten-base-shape-test
        t out/gpu/gpu-user-dir-test
        t out/gpu/null-descriptor-test
        t out/gpu/upscaler-support-test
        mkdir -p out/macos-buffer-bounds
        t out/gpu/buffer-bounds-test out/macos-buffer-bounds
        mkdir -p out/mac-inventory
        t out/gpu/buffer-dead-resources-test
        t out/gpu/host-import-arena-test
        t out/gpu/vertex-pulling-pipeline-test
        t out/gpu/shader-permutation-info-test
        t out/gpu/push-userdata-test out/push-userdata.spv
        t spirv-val --target-env vulkan1.3 out/push-userdata.spv
        # Depth-bounds emulation shaders: 8 permutations + 2 entry-phi cases, each validated.
        rm -rf out/depth-bounds-shader && mkdir -p out/depth-bounds-shader
        t out/gpu/depth-bounds-shader-test out/depth-bounds-shader
        depth_spv=(out/depth-bounds-shader/*.spv)
        [[ -n $dry ]] || [[ ${#depth_spv[@]} == 10 ]] || { echo "depth-bounds-shader-test wrote ${#depth_spv[@]} SPIR-V files, expected 10" >&2; exit 1; }
        for spv in "${depth_spv[@]}"; do t spirv-val --target-env vulkan1.3 "$spv"; done
        rm -rf out/shader-inputs-modules && mkdir -p out/shader-inputs-modules
        t out/gpu/shader-inputs-test out/shader-inputs-modules
        input_spv=(out/shader-inputs-modules/*.spv)
        [[ -n $dry ]] || [[ ${#input_spv[@]} == 12 ]] || { echo "shader-inputs-test wrote ${#input_spv[@]} SPIR-V files, expected 12" >&2; exit 1; }
        for spv in "${input_spv[@]}"; do t spirv-val --target-env vulkan1.3 "$spv"; done
        t out/gpu/flatten-base-shape-test
        # Default allocated descriptor sets only; --push reproduces a MoltenVK bug and exits 2.
        BB_DEPS_PREFIX="$deps" t bash tests/macos_descriptor_sizes/run.sh
        BB_DEPS_PREFIX="$deps" t bash tests/test_fp64_policy.sh
        BB_DEPS_PREFIX="$deps" BB_GPU_BUILD="$PWD/out/gpu" t sh tests/test_buffer_atomic_wrap.sh
        BB_DEPS_PREFIX="$deps" BB_GPU_BUILD="$PWD/out/gpu" t sh tests/test_lds_subword.sh
        BB_DEPS_PREFIX="$deps" t sh tests/test_geometry_runtime_info.sh
        BB_DEPS_PREFIX="$deps" BB_GPU_BUILD="$PWD/out/gpu" t bash tests/macos_fp16/run.sh
        BB_DEPS_PREFIX="$deps" BB_GPU_BUILD="$PWD/out/gpu" t sh tests/macos_pkrtz/run.sh
        BB_DEPS_PREFIX="$deps" BB_GPU_BUILD="$PWD/out/gpu" BB_GPU_THIRD_PARTY="$PWD/gpu/third_party" \
            t bash tests/macos_gs/run.sh
        # GPU library unit tests: Apple paths (PM4 parsing, feature checks, views, scheduler,
        # vertex fetch, null buffers, rect interface, overlay) and the renderer's cross-platform
        # tests (motion, UI composition, scene resolution, TAA, camera motion).
        unit_tests=(cmd-snapshot-test cond-exec-test strmout-update-test equeue-concurrency-test feature-checks-test image-view-storage-test
            submit-info-bounds-test vertex-fetch-translation-test buffer-null-test rect-interface-test
            fence-address-test overlay-concurrency-test depth-bounds-probe-test motion-history-test
            motion-shader-test
            ui-composition-test scene-resolution-test taa-shader-test camera-motion-test)
        ninja -C out/gpu "${ninja_jobs[@]}" "${unit_tests[@]}"
        for unit_test in "${unit_tests[@]}"; do sign_darwin "out/gpu/$unit_test"; done
        t out/gpu/cmd-snapshot-test
        t out/gpu/cond-exec-test
        t out/gpu/strmout-update-test
        t out/gpu/equeue-concurrency-test
        t out/gpu/feature-checks-test
        t out/gpu/image-view-storage-test
        t out/gpu/submit-info-bounds-test
        t out/gpu/vertex-fetch-translation-test
        t out/gpu/fence-address-test
        t out/gpu/overlay-concurrency-test
        t out/gpu/rect-interface-test
        t out/gpu/depth-bounds-probe-test
        rm -rf out/buffer-null && mkdir -p out/buffer-null
        t out/gpu/buffer-null-test out/buffer-null
        t spirv-val --target-env vulkan1.3 out/buffer-null/null-buffer.spv
        t spirv-val --target-env vulkan1.3 out/buffer-null/valid-buffer.spv
        t out/gpu/motion-history-test
        rm -rf out/motion-shaders && mkdir -p out/motion-shaders
        t out/gpu/motion-shader-test out/motion-shaders
        for spv in out/motion-shaders/*.spv; do t spirv-val --target-env vulkan1.3 "$spv"; done
        t out/gpu/ui-composition-test
        t out/gpu/scene-resolution-test
        t out/gpu/taa-shader-test
        t out/gpu/camera-motion-test
        # GCN2 buffer conformance (Opus verification's suite) against this tree's GPU library:
        # exit 0 only (FAIL, INVALID-SPIRV, CRASH and harness errors exit non-zero).
        BB_GPU_BUILD="$PWD/out/gpu" BB_GPU_THIRD_PARTY="$PWD/gpu/third_party" \
            bash tests/gcn_buffer_conformance/build.sh --out "$PWD/out/gcnconf"
        BB_GPU_USER_DIR="$PWD/out/gcnconf/user" t "$PWD/out/gcnconf/gcn-conformance-dylib" \
            --out "$PWD/out/gcnconf" --label shared
        # Standalone qualification probes and runtime pieces.
        t sh tests/test_guest_alloc_macos.sh
        t sh tests/test_memory_clamp.sh
        t sh tests/test_memory_cpu_alias_macos.sh
        t sh tests/test_audio_macos.sh
        t sh tests/test_guest_heap_lock_macos.sh
        t sh tests/test_low_map_trace_macos.sh
        t sh tests/test_perf_summary.sh
        t sh tests/test_guest_entry_macos.sh
        t sh tests/test_cpu_profile_macos.sh
        t sh tests/test_hle_trace_macos.sh
        BB_DEPS_PREFIX="$deps" t bash tests/macos_external_host/run.sh
        BB_DEPS_PREFIX="$deps" t bash tests/macos_metalfx/signature.sh
        BB_DEPS_PREFIX="$deps" t bash tests/macos_metalfx/run.sh
        # The production module (gpu/shim/macos_metalfx.mm) as the provider, incl. off-thread teardown.
        BB_DEPS_PREFIX="$deps" t bash tests/macos_metalfx/run.sh --module
        BB_DEPS_PREFIX="$deps" t bash tests/macos_metalfx/jitter.sh
        BB_GPU_BUILD="$PWD/out/gpu" t sh tests/macos_metalfx/library.sh
        # Scaled presets: tonemapped RGBA8 content in larger aligned textures, both output formats.
        BB_DEPS_PREFIX="$deps" t bash tests/macos_metalfx/run.sh --ldr-padded
        # Every tests/test_*.py (a skipped test fails).
        t bash tests/run_python_tests.sh
        # BB_PERF's GPU numbers rely on per-submission timestamps; qualify them on this device.
        BB_DEPS_PREFIX="$deps" t bash tests/macos_perf/timestamps.sh
        t out/gpu/macos-perf-test
        BB_GPU_BUILD="$PWD/out/gpu" t sh tests/macos_float_modes/production.sh
        "$CC" "${platform_flags[@]}" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -Isrc \
            tests/test_compat_mac.c src/compat_mac.c -o out/compat-mac-test
        sign_darwin out/compat-mac-test
        t out/compat-mac-test
        "$CC" "${platform_flags[@]}" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -Isrc \
            tests/test_macos_ui.c src/macos_ui.c -o out/macos-ui-test
        sign_darwin out/macos-ui-test
        t out/macos-ui-test
        "$CC" "${platform_flags[@]}" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread \
            "${includes[@]}" -I. -Isrc tests/test_fault_mac.c src/runtime_cpu_profile.c "${compat[@]}" \
            -Wl,-dead_strip -o out/fault-mac-test
        sign_darwin out/fault-mac-test
        t out/fault-mac-test
        read -r -a media_flags <<< "$(pkg-config --cflags --libs libavformat libavcodec libavutil)"
        "${CXX:-clang++}" "${platform_flags[@]}" -std=c++23 -O2 -g -Wall -Wextra -Werror \
            -I. tests/test_media_mac.cpp "${media_flags[@]}" "${caps_link[@]}" -o out/media-mac-test
        sign_darwin out/media-mac-test
        t out/media-mac-test
        read -r -a surface_flags <<< "$(pkg-config --cflags --libs vulkan sdl3 fmt)"
        "$CC" "${platform_flags[@]}" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -Isrc \
            -c src/macos_ui.c -o out/macos-ui-surface.o
        "${CXX:-clang++}" "${platform_flags[@]}" -std=c++23 -O2 -g -Wall -Wextra -Werror \
            -Wno-unused-parameter -Wno-unused-function -pthread \
            -Isrc -Igpu/shim -Igpu/shadps4 tests/test_macos_surface.cpp gpu/shim/window.cpp \
            gpu/shadps4/video_core/renderer_vulkan/vk_platform.cpp gpu/shim/common/logging/log.cpp \
            gpu/shim/common/assert.cpp gpu/shadps4/common/path_util.cpp out/macos-ui-surface.o \
            "${surface_flags[@]}" "${caps_link[@]}" -o out/macos-surface-test
        sign_darwin out/macos-surface-test
        t out/macos-surface-test
        t sh tests/test_memory_macos.sh
        t sh tests/test_memory_lifetime_macos.sh
        t sh tests/test_avplayer_lifecycle_macos.sh
        t sh tests/macos_spike/test_tls_runtime.sh
        t sh tests/macos_spike/test_hle_stack.sh
        t bash tests/test_savedata_macos.sh
        t bash tests/test_heavy.sh
        t sh tests/test_sync_diagnostics_macos.sh
        t python3 tests/test_restart_macos.py
        # Last: every test file under tests/ is run above or excluded with a reason.
        python3 tools/macos/check_test_wiring.py
    fi
    "$CC" "${platform_flags[@]}" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread "${includes[@]}" -I. -Isrc tests/test_pad.c "${libraries[@]}" "${caps_link[@]}" -o out/pad-test
    sign_darwin out/pad-test
    t out/pad-test
    "$CC" "${platform_flags[@]}" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread "${includes[@]}" -I. -Isrc tests/test_runtime.c "${runtime[@]}" out/libatrac9.a -lm "${gpu[@]}" "${libraries[@]}" -o out/runtime-test
    sign_darwin out/runtime-test
    if [[ $(uname -s) == Darwin ]]; then
        python3 packaging/macos/check-exports.py out/gpu/libbbgpu.dylib out/runtime-test
    fi
    t out/runtime-test
    "$CC" "${platform_flags[@]}" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -Isrc tests/test_file_mods.c -o out/file-mods-test
    sign_darwin out/file-mods-test
    t out/file-mods-test
    "$CC" "${platform_flags[@]}" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread "${includes[@]}" -I. -Isrc tests/test_sema.c "${runtime[@]}" out/libatrac9.a -lm "${gpu[@]}" "${libraries[@]}" -o out/sema-test
    sign_darwin out/sema-test
    if [[ $(uname -s) == Darwin ]]; then
        python3 packaging/macos/check-exports.py out/gpu/libbbgpu.dylib out/sema-test
    fi
    t out/sema-test
    "$CC" "${platform_flags[@]}" -std=c11 -D_GNU_SOURCE -O2 -g -Wall -Wextra -Werror -I. -Isrc tests/test_content.c src/runtime_content.c -o out/content-test
    sign_darwin out/content-test
    t out/content-test
fi
