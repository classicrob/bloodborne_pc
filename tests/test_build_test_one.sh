#!/usr/bin/env bash
# build.sh's test runner (the block between its "--- test runner" markers) under --test-one, with
# fake tests: the first failure's exit is the result, a failed test's outputs are not checked
# again, a wrong output count fails with the summary, an unselected test's output directory is
# left alone, an unknown name lists the tests, and the full --test still stops at a bad count.
set -euo pipefail
cd "$(dirname "$0")/.."
runner=$(mktemp); dir=$(mktemp -d); trap 'rm -rf "$runner" "$dir"' EXIT
sed -n '/# --- test runner (tests\/test_build_test_one.sh sources this block) ---/,/# --- end test runner ---/p' \
    build.sh > "$runner"
[[ $(wc -l < "$runner") -gt 20 ]] || { echo "build.sh test runner block not found" >&2; exit 1; }
fail() { echo "build.sh --test-one test FAILED: $*" >&2; exit 1; }
# A fake test target: out/gpu/<name> writing N files to DIR and exiting STATUS.
fake() {  # fake NAME FILES STATUS
    mkdir -p "$dir/out/gpu"
    printf '#!/bin/sh\nfor i in $(seq %d); do [ "$1" ] && : > "$1/$i.spv"; done\nexit %d\n' "$2" "$3" \
        > "$dir/out/gpu/$1"
    chmod +x "$dir/out/gpu/$1"
}
# scenario ONE FILES STATUS: --test-one ONE over the depth-bounds block, in a subshell;
# prints its output, returns its exit status.
scenario() {
    (cd "$dir"; dry='' one=$1 one_ran=0 one_failed=0 one_last=0 one_seen=()
     sign_darwin() { :; }
     spirv-val() { echo "$*" >> "$dir/spirv-calls"; }  # counts t's spirv-val follow-ups
     : > "$dir/spirv-calls"
     source "$runner"
     fake depth-bounds-shader-test "$2" "$3"; fake other-test 0 0
     mkdir -p out/depth-bounds-shader && : > out/depth-bounds-shader/sentinel
     t out/gpu/other-test
     fresh_dir out/gpu/depth-bounds-shader-test out/depth-bounds-shader
     t out/gpu/depth-bounds-shader-test out/depth-bounds-shader
     depth_spv=(out/depth-bounds-shader/*.spv)
     outputs_ok ${#depth_spv[@]} 10 "depth-bounds-shader-test wrote ${#depth_spv[@]} SPIR-V files, expected 10"
     for spv in "${depth_spv[@]}"; do t spirv-val --target-env vulkan1.3 "$spv"; done
     echo "spirv-val calls: $(wc -l < "$dir/spirv-calls" | tr -d ' ')"
     [[ -e out/depth-bounds-shader/sentinel ]] && echo "sentinel kept" || echo "sentinel removed"
     test_one_summary) 2>&1
}
set +e
out=$(scenario depth-bounds-shader-test 0 23); status=$?
set -e
((status == 23)) || fail "producer exit 23 with no outputs: exit $status, expected 23: $out"
[[ $out == *'test-one: depth-bounds-shader-test: 1 run(s), exit 23'* ]] || fail "no summary for exit 23: $out"
[[ $out != *'output check'* && $out == *'spirv-val calls: 0'* ]] ||
    fail "a failed test's missing outputs were checked again: $out"
echo "PASS producer exit 23, no outputs: exit 23, summary, no output check, no spirv-val"
set +e
out=$(scenario depth-bounds-shader-test 3 0); status=$?
set -e
((status == 1)) || fail "wrong output count: exit $status, expected 1: $out"
[[ $out == *'test-one: exit 1: output check: depth-bounds-shader-test wrote 3 SPIR-V files, expected 10'* &&
   $out == *'test-one: depth-bounds-shader-test: 4 run(s), exit 1'* ]] || fail "wrong count not summarised: $out"
[[ $out == *'sentinel removed'* && $out == *'spirv-val calls: 3'* ]] ||
    fail "the selected test's output directory was not fresh, or its outputs not validated: $out"
echo "PASS producer exit 0, 3 of 10 outputs: exit 1 with the output check and the summary; 3 validated"
set +e
out=$(scenario depth-bounds-shader-test 10 0); status=$?
set -e
((status == 0)) && [[ $out == *'spirv-val calls: 10'* && $out == *'test-one: depth-bounds-shader-test: 11 run(s), exit 0'* ]] ||
    fail "all 10 outputs: exit $status: $out"
echo "PASS producer exit 0, 10 outputs: each validated by spirv-val (11 runs), exit 0"
set +e
out=$(scenario other-test 0 0); status=$?
set -e
((status == 0)) && [[ $out == *'sentinel kept'* && $out == *'spirv-val calls: 0'* ]] ||
    fail "unselected test's outputs touched or validated (exit $status): $out"
echo "PASS another test selected: the depth-bounds output directory and its files are left alone"
set +e
out=$(scenario no-such-test 0 0); status=$?
set -e
((status == 1)) && [[ $out == *"no test command names 'no-such-test'"* && $out == *'out/gpu/other-test'* &&
   $out == *'sentinel kept'* ]] || fail "unknown name (exit $status): $out"
echo "PASS unknown name: exit 1, lists the tests, touches no output directory"
set +e
out=$(cd "$dir"; dry='' one='' one_ran=0 one_failed=0 one_last=0 one_seen=()
      sign_darwin() { :; }; source "$runner"; fake depth-bounds-shader-test 3 0
      t out/gpu/depth-bounds-shader-test out/depth-bounds-shader
      outputs_ok 3 10 "wrote 3, expected 10"; echo "went on" 2>&1); status=$?
set -e
((status == 1)) && [[ $out != *'went on'* ]] || fail "full --test went on past a bad output count (exit $status): $out"
echo "PASS full --test: a bad output count still stops it (exit 1)"
