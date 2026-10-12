#!/usr/bin/env bash
# Geometry emulation (Apple) checks: compile-negative controls, the runtime conformance suite on
# MoltenVK and its four negative controls (each must make every case that could show it FAIL),
# and the device's provoking vertex with its control.
#   BB_GPU_BUILD=<tree>/out/gpu BB_GPU_THIRD_PARTY=<tree>/gpu/third_party tests/macos_gs/run.sh
# The GPU library must be built from this checkout's tree (the tools compile its recompiler).
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
root=$(cd -- "$here/../.." && pwd)
out=${BB_GS_TEST_OUT:-$root/out/macos_gs}
deps=${BB_DEPS_PREFIX:-$HOME/bbport-deps/x86_64}
mkdir -p "$out"
"$here/build.sh" > "$out/build.log" 2>&1 || { tail -20 "$out/build.log"; exit 1; }
icd="$deps/share/vulkan/icd.d/MoltenVK_icd.json"
run() { arch -x86_64 env VK_DRIVER_FILES="$icd" "$@"; }

fail=0
# Compile-negative controls: what the ES/GS compute roles cannot run must stop loudly, by name.
if run "$out/compile_roles" --negative - 0 "$out/negative" > "$out/negative.log" 2>&1; then
    echo "PASS: compile-negative controls ($(grep -c 'LOUD (expected)' "$out/negative.log") loud as expected)"
else
    echo "FAIL: compile-negative controls"; cat "$out/negative.log"; fail=1
fi
expect() { # label, expected "N of M cases passed", log
    local got
    got=$(grep -o '[0-9]* of [0-9]* cases passed' "$3" || echo "no summary")
    if [[ $got == "$2" ]]; then echo "PASS: $1 ($got)"; else echo "FAIL: $1 (got: $got, want: $2)"; fail=1; fi
}
run "$out/conformance" "$out" > "$out/conformance.log" 2>&1 || true
expect "conformance" "39 of 39 cases passed" "$out/conformance.log"
run "$out/conformance" "$out" --control base-vertex > "$out/control-base-vertex.log" 2>&1 || true
expect "control base-vertex (data; only the overflow case passes)" "1 of 39 cases passed" "$out/control-base-vertex.log"
run "$out/conformance" "$out" --control reverse-order > "$out/control-reverse-order.log" 2>&1 || true
expect "control reverse-order (only the 4 empty cases pass)" "4 of 39 cases passed" "$out/control-reverse-order.log"
run "$out/conformance" "$out" --control rotate-provoking > "$out/control-rotate-provoking.log" 2>&1 || true
expect "control rotate-provoking (empty, line and point cases pass)" "27 of 39 cases passed" "$out/control-rotate-provoking.log"
run "$out/conformance" "$out" --control so-lane > "$out/control-so-lane.log" 2>&1 || true
expect "control so-lane (every stream-out case fails)" "22 of 39 cases passed" "$out/control-so-lane.log"
# The device's flat vertex is Vulkan's FIRST provoking vertex (lists, strips incl. odd triangles
# and restarts, fans): what lowering GCN's flat P0 reads to Flat relies on.
run "$out/conformance" "$out" --provoking > "$out/provoking.log" 2>&1 || true
expect "provoking vertex" "4 of 4 cases passed" "$out/provoking.log"
run "$out/conformance" "$out" --provoking --control provoking-last > "$out/control-provoking-last.log" 2>&1 || true
expect "control provoking-last (every case fails)" "0 of 4 cases passed" "$out/control-provoking-last.log"
exit $fail
