#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")"
if [[ -n ${BB_TEST_SKIP:-} && ${1:-} != --test-keep-going ]]; then
    echo 'BB_TEST_SKIP is for build.sh --test-keep-going only (a skip is listed in its summary)' >&2
    exit 2
fi
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
    for source in src/probe.c "${runtime[@]}" src/vulkan_smoke.c src/macos_activity.c; do
        object=out/probe-obj/$(basename "${source%.c}").o
        "$CC" "${platform_flags[@]}" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread "${includes[@]}" -I. -Isrc \
            -c "$source" -o "$object"
        probe_objects+=("$object")
    done
    "$CC" "${platform_flags[@]}" -g -pthread "${probe_link_flags[@]}" "${probe_objects[@]}" out/libatrac9.a -lm \
        "${gpu[@]}" "${libraries[@]}" -lobjc -framework Foundation -o out/bb-probe
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
if [[ ${1:-} == --test-one && -z ${2:-} ]]; then
    echo 'usage: build.sh --test-one <test target or script, as build.sh --test runs it>' >&2
    exit 2
fi
if [[ ${1:-} == --test || ${1:-} == --test-dry || ${1:-} == --test-one || ${1:-} == --test-keep-going ]]; then
    # --test-dry builds, links and signs everything --test would run, and runs the static checks
    # (export gate, test wiring), but executes no test: t checks each test binary exists and is
    # signed, and each test script exists. Run it before merging a change to build.sh or a test
    # target. Tests that compile inside their own scripts are only checked to exist.
    # --test-one NAME builds only the test target NAME (with the library and probe, as every
    # build) and runs each test command naming it (an argument NAME or ending in /NAME: a target,
    # out/gpu/x-test, a script) exactly as --test does: same environment, signing and directories,
    # with the checks of its outputs (spirv-val, file counts). Each run prints "test-one: exit N";
    # the exit is the first failure's, or 0. A NAME no test command has is an error that lists them.
    # --test-keep-going (the gate for previews and merge requests) runs every test even after a
    # failure: each exit goes to out/test-results.txt, each failure is said as it happens, and the
    # end prints run/passed/failed/skipped and the failures; the exit is 1 if any test failed, 3 if
    # none failed but some were skipped (NOT A FULL GATE), 0 only when every test ran and passed.
    # BB_TEST_SKIP=SUBSTRING skips the test commands containing it, listed as skipped. A failing
    # build or non-test step still stops it (non-zero, no summary).
    dry='' one='' keep=''
    if [[ $1 == --test-dry ]]; then dry=1; fi
    if [[ $1 == --test-one ]]; then one=$2; fi
    if [[ $1 == --test-keep-going ]]; then keep=1; mkdir -p out; : > out/test-results.txt; fi
    one_ran=0 one_failed=0 one_last=0 one_seen=() keep_ran=0 keep_skipped=() keep_failed=()
    # --- test runner (tests/test_build_test_one.sh and test_build_keep_going.sh source this block) ---
    names() {  # does an argument name the --test-one test?
        local arg
        for arg; do [[ $arg == "$one" || $arg == */"$one" ]] && return 0; done
        return 1
    }
    want() {  # build this test (--test-one: only the one, and what the Python tests launch for them)
        [[ -z $one ]] || names "$@" ||
            { [[ $one == run_python_tests.sh || $one == */run_python_tests.sh ]] &&
              [[ $1 == out/runtime-test || $1 == out/sema-test || $1 == out/content-test ]]; }
    }
    only_wanted() {  # prints the GPU test targets to build: all, or --test-one's
        local target
        for target; do if want "out/gpu/$target"; then echo "$target"; fi; done
    }
    # Skip a check of a test's outputs: --test-dry, or --test-one when that test was not selected
    # or failed (its own exit is the failure; its missing outputs are not a second one).
    unchecked() { [[ -n $dry ]] || [[ -n $one$keep && $one_last == 0 ]]; }
    fresh_dir() {  # fresh_dir TEST DIR: an empty output directory for TEST, if TEST runs
        if want "$1"; then rm -rf "$2" && mkdir -p "$2"; fi
    }
    outputs_ok() {  # outputs_ok COUNT EXPECTED MESSAGE: the output count of the test just run
        unchecked || (($1 == $2)) && return 0
        echo "$3" >&2
        if [[ -n $keep ]]; then
            ((++keep_ran))  # the check is a test of its own: run, and failed
            keep_failed+=("output check: $3")
            echo "output check: $3" >> out/test-results.txt
            echo "test-keep-going: FAIL output check: $3"
            return 0
        fi
        [[ -n $one ]] || exit 1
        echo "test-one: exit 1: output check: $3"
        if ((!one_failed)); then one_failed=1; fi
    }
    test_keep_going_summary() {  # --test-keep-going's end: counts and failures; exit 1 on a
        # failure, 3 when tests were skipped (passed, but not a full gate), 0 only for all run and passed
        local passed=$((keep_ran - ${#keep_failed[@]}))
        echo "test-keep-going: ${keep_ran} run, ${passed} passed, ${#keep_failed[@]} failed, ${#keep_skipped[@]} skipped (out/test-results.txt)"
        if ((${#keep_skipped[@]})); then printf '  skipped: %s\n' "${keep_skipped[@]}"; fi
        if ((${#keep_failed[@]})); then
            printf '  FAILED: %s\n' "${keep_failed[@]}"
            exit 1
        fi
        if ((${#keep_skipped[@]})); then
            echo "test-keep-going: NOT A FULL GATE: ${#keep_skipped[@]} skipped (exit 3)"
            exit 3
        fi
        exit 0
    }
    test_one_summary() {  # --test-one's end: its runs, or the test commands when none was named
        if ((!one_ran)); then
            echo "test-one: no test command names '$one'. The test commands:" >&2
            printf '  %s\n' "${one_seen[@]}" >&2
            exit 1
        fi
        echo "test-one: $one: $one_ran run(s), exit $one_failed"
        exit "$one_failed"
    }
    t() {
        if [[ -n $keep ]]; then
            if [[ $1 == spirv-val ]] && ((!one_last)); then return 0; fi  # its producer failed
            if [[ -n ${BB_TEST_SKIP:-} && "$*" == *"$BB_TEST_SKIP"* ]]; then
                keep_skipped+=("$*")
                echo "skipped (BB_TEST_SKIP): $*" >> out/test-results.txt
                echo "test-keep-going: SKIPPED (BB_TEST_SKIP=$BB_TEST_SKIP): $*"
                [[ $1 == spirv-val ]] || one_last=0
                return 0
            fi
            local rc=0
            "$@" || rc=$?
            ((++keep_ran))
            echo "exit $rc: $*" >> out/test-results.txt
            if ((rc)); then
                keep_failed+=("exit $rc: $*")
                echo "test-keep-going: FAIL exit $rc: $*"
            fi
            if [[ $1 != spirv-val ]]; then one_last=$((rc == 0)); fi
            return 0
        fi
        if [[ -n $one ]]; then
            if [[ $1 == spirv-val ]]; then
                ((one_last)) || return 0  # checks the outputs of the test before it
            elif names "$@"; then
                one_last=1
            else
                one_last=0 one_seen+=("$*")
                return 0
            fi
            local rc=0
            "$@" || rc=$?
            ((++one_ran))
            echo "test-one: exit $rc: $*"
            if ((rc && !one_failed)); then one_failed=$rc; fi
            if ((rc)) && [[ $1 != spirv-val ]]; then one_last=0; fi  # no checks of a failed test's outputs
            return 0
        fi
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
    # --- end test runner ---
    if [[ $(uname -s) == Darwin ]]; then
        # The GPU library has no default user directory on macOS: fixtures get a scratch one.
        export BB_GPU_USER_DIR="$PWD/out/gpu-test-user"
        gpu_tests=($(only_wanted null-descriptor-test upscaler-support-test buffer-bounds-test buffer-dead-resources-test host-import-arena-test vertex-pulling-pipeline-test shader-permutation-info-test gpu-user-dir-test push-userdata-test macos-perf-test depth-bounds-shader-test shader-inputs-test flatten-base-shape-test))
        if ((${#gpu_tests[@]})); then
            ninja -C out/gpu "${ninja_jobs[@]}" "${gpu_tests[@]}"
            sign_darwin "${gpu_tests[@]/#/out/gpu/}"
        fi
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
        fresh_dir out/gpu/depth-bounds-shader-test out/depth-bounds-shader
        t out/gpu/depth-bounds-shader-test out/depth-bounds-shader
        depth_spv=(out/depth-bounds-shader/*.spv)
        outputs_ok ${#depth_spv[@]} 10 "depth-bounds-shader-test wrote ${#depth_spv[@]} SPIR-V files, expected 10"
        for spv in "${depth_spv[@]}"; do t spirv-val --target-env vulkan1.3 "$spv"; done
        fresh_dir out/gpu/shader-inputs-test out/shader-inputs-modules
        t out/gpu/shader-inputs-test out/shader-inputs-modules
        input_spv=(out/shader-inputs-modules/*.spv)
        outputs_ok ${#input_spv[@]} 12 "shader-inputs-test wrote ${#input_spv[@]} SPIR-V files, expected 12"
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
        BB_DEPS_PREFIX="$deps" BB_GPU_BUILD="$PWD/out/gpu" t sh tests/macos_ufloat_fold/run.sh
        BB_DEPS_PREFIX="$deps" BB_GPU_BUILD="$PWD/out/gpu" BB_GPU_THIRD_PARTY="$PWD/gpu/third_party" \
            t bash tests/macos_gs/run.sh
        # GPU library unit tests: Apple paths (PM4 parsing, feature checks, views, scheduler,
        # vertex fetch, null buffers, rect interface, overlay) and the renderer's cross-platform
        # tests (motion, UI composition, scene resolution, TAA, camera motion).
        unit_tests=(cmd-snapshot-test cond-exec-test label-check-test capture-quiesce-test flip-publish-test videoout-lifecycle-test strmout-update-test equeue-concurrency-test feature-checks-test image-view-storage-test
            submit-info-bounds-test vertex-fetch-translation-test buffer-null-test rect-interface-test
            fence-address-test fill-clear-device-test capture-images-test scheduler-close-test scheduler-async-submit-test overlay-concurrency-test depth-bounds-probe-test replay-capture-test
            slot-vector-test
            barrier-alias-test
            motion-history-test
            motion-shader-test
            ui-composition-test fill-clear-test scene-resolution-test taa-shader-test camera-motion-test)
        unit_tests=($(only_wanted "${unit_tests[@]}"))
        if ((${#unit_tests[@]})); then
            ninja -C out/gpu "${ninja_jobs[@]}" "${unit_tests[@]}"
            for unit_test in "${unit_tests[@]}"; do sign_darwin "out/gpu/$unit_test"; done
        fi
        t out/gpu/cmd-snapshot-test
        t out/gpu/cond-exec-test
        t out/gpu/label-check-test
        t out/gpu/capture-quiesce-test
        t out/gpu/flip-publish-test
        t out/gpu/videoout-lifecycle-test
        t out/gpu/strmout-update-test
        t out/gpu/equeue-concurrency-test
        t out/gpu/feature-checks-test
        t out/gpu/image-view-storage-test
        t out/gpu/submit-info-bounds-test
        t out/gpu/vertex-fetch-translation-test
        t out/gpu/fence-address-test
        t out/gpu/fill-clear-device-test
        t out/gpu/capture-images-test
        t out/gpu/scheduler-close-test
        t out/gpu/scheduler-async-submit-test
        t out/gpu/overlay-concurrency-test
        t out/gpu/rect-interface-test
        t out/gpu/depth-bounds-probe-test
        t out/gpu/replay-capture-test
        t out/gpu/slot-vector-test
        t out/gpu/barrier-alias-test
        fresh_dir out/gpu/buffer-null-test out/buffer-null
        t out/gpu/buffer-null-test out/buffer-null
        t spirv-val --target-env vulkan1.3 out/buffer-null/null-buffer.spv
        t spirv-val --target-env vulkan1.3 out/buffer-null/valid-buffer.spv
        t out/gpu/motion-history-test
        fresh_dir out/gpu/motion-shader-test out/motion-shaders
        t out/gpu/motion-shader-test out/motion-shaders
        for spv in out/motion-shaders/*.spv; do t spirv-val --target-env vulkan1.3 "$spv"; done
        t out/gpu/ui-composition-test
        t out/gpu/fill-clear-test
        t out/gpu/scene-resolution-test
        t out/gpu/taa-shader-test
        t out/gpu/camera-motion-test
        # GCN2 buffer conformance (Opus verification's suite) against this tree's GPU library:
        # exit 0 only (FAIL, INVALID-SPIRV, CRASH and harness errors exit non-zero).
        if want "$PWD/out/gcnconf/gcn-conformance-dylib"; then
            BB_GPU_BUILD="$PWD/out/gpu" BB_GPU_THIRD_PARTY="$PWD/gpu/third_party" \
                bash tests/gcn_buffer_conformance/build.sh --out "$PWD/out/gcnconf"
            sign_darwin out/gcnconf/gcn-conformance-dylib
        fi
        BB_GPU_USER_DIR="$PWD/out/gcnconf/user" t "$PWD/out/gcnconf/gcn-conformance-dylib" \
            --out "$PWD/out/gcnconf" --label shared
        # Standalone qualification probes and runtime pieces.
        t sh tests/test_guest_alloc_macos.sh
        t bash tests/test_macos_activity.sh
        t sh tests/test_alarm.sh
        t sh tests/test_memory_clamp.sh
        t sh tests/test_toggle_file.sh
        t sh tests/test_memory_cpu_alias_macos.sh
        t sh tests/test_memory_inventory_macos.sh
        t sh tests/test_audio_macos.sh
        t sh tests/test_audio_qos_macos.sh
        t sh tests/test_player_position_macos.sh
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
        # BB_PERF's GPU numbers rely on per-submission timestamps; qualify them on this device.
        # It takes the GPU exclusively (heavy.sh): it waits for running games instead of measuring
        # a shared GPU.
        BB_DEPS_PREFIX="$deps" t tools/macos/heavy.sh --gpu exclusive "timestamp gate" -- \
            bash tests/macos_perf/timestamps.sh
        t out/gpu/macos-perf-test
        BB_GPU_BUILD="$PWD/out/gpu" t sh tests/macos_float_modes/production.sh
        if want out/compat-mac-test; then
            "$CC" "${platform_flags[@]}" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -Isrc \
                tests/test_compat_mac.c src/compat_mac.c -o out/compat-mac-test
            sign_darwin out/compat-mac-test
        fi
        t out/compat-mac-test
        if want out/macos-ui-test; then
            "$CC" "${platform_flags[@]}" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -Isrc \
                tests/test_macos_ui.c src/macos_ui.c -o out/macos-ui-test
            sign_darwin out/macos-ui-test
        fi
        t out/macos-ui-test
        if want out/fault-mac-test; then
            "$CC" "${platform_flags[@]}" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread \
                "${includes[@]}" -I. -Isrc tests/test_fault_mac.c src/runtime_cpu_profile.c "${compat[@]}" \
                -Wl,-dead_strip -o out/fault-mac-test
            sign_darwin out/fault-mac-test
        fi
        t out/fault-mac-test
        if want out/media-mac-test; then
            read -r -a media_flags <<< "$(pkg-config --cflags --libs libavformat libavcodec libavutil)"
            "${CXX:-clang++}" "${platform_flags[@]}" -std=c++23 -O2 -g -Wall -Wextra -Werror \
                -I. tests/test_media_mac.cpp "${media_flags[@]}" "${caps_link[@]}" -o out/media-mac-test
            sign_darwin out/media-mac-test
        fi
        t out/media-mac-test
        if want out/macos-surface-test; then
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
        fi
        t out/macos-surface-test
        t sh tests/test_memory_macos.sh
        t sh tests/test_memory_lifetime_macos.sh
        t sh tests/test_avplayer_lifecycle_macos.sh
        t sh tests/macos_spike/test_tls_runtime.sh
        t sh tests/macos_spike/test_hle_stack.sh
        t bash tests/test_savedata_macos.sh
        t bash tests/test_heavy.sh
        t bash tests/test_build_test_one.sh
        t bash tests/test_build_keep_going.sh
        t sh tests/test_sync_diagnostics_macos.sh
        t sh tests/test_cond_wake_check_macos.sh
        t python3 tests/test_restart_macos.py
        t python3 tests/test_motion_agree.py
        t python3 tests/test_gpu_modified_setters.py
        # Last: every test file under tests/ is run above or excluded with a reason.
        [[ -n $one ]] || python3 tools/macos/check_test_wiring.py
    fi
    if want out/pad-test; then
        "$CC" "${platform_flags[@]}" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread "${includes[@]}" -I. -Isrc tests/test_pad.c "${libraries[@]}" "${caps_link[@]}" -o out/pad-test
        sign_darwin out/pad-test
    fi
    t out/pad-test
    if want out/runtime-test; then
        "$CC" "${platform_flags[@]}" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread "${includes[@]}" -I. -Isrc tests/test_runtime.c "${runtime[@]}" out/libatrac9.a -lm "${gpu[@]}" "${libraries[@]}" -o out/runtime-test
        sign_darwin out/runtime-test
        if [[ $(uname -s) == Darwin ]]; then
            python3 packaging/macos/check-exports.py out/gpu/libbbgpu.dylib out/runtime-test
        fi
    fi
    t out/runtime-test
    if want out/file-mods-test; then
        "$CC" "${platform_flags[@]}" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -Isrc tests/test_file_mods.c -o out/file-mods-test
        sign_darwin out/file-mods-test
    fi
    t out/file-mods-test
    if want out/sema-test; then
        "$CC" "${platform_flags[@]}" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread "${includes[@]}" -I. -Isrc tests/test_sema.c "${runtime[@]}" out/libatrac9.a -lm "${gpu[@]}" "${libraries[@]}" -o out/sema-test
        sign_darwin out/sema-test
        if [[ $(uname -s) == Darwin ]]; then
            python3 packaging/macos/check-exports.py out/gpu/libbbgpu.dylib out/sema-test
        fi
    fi
    t out/sema-test
    if want out/content-test; then
        "$CC" "${platform_flags[@]}" -std=c11 -D_GNU_SOURCE -O2 -g -Wall -Wextra -Werror -I. -Isrc tests/test_content.c src/runtime_content.c -o out/content-test
        sign_darwin out/content-test
    fi
    t out/content-test
    if [[ $(uname -s) == Darwin ]]; then
        # Every tests/test_*.py (a skipped test fails). Last: test_runtime.py and test_sema.py run
        # out/runtime-test and out/sema-test, which must be this build's (a stale pair from an
        # older --test cannot load the current libbbgpu, and stopped the run before the rebuild).
        t bash tests/run_python_tests.sh
    fi
    if [[ -n $one ]]; then test_one_summary; fi
    if [[ -n $keep ]]; then test_keep_going_summary; fi
fi
