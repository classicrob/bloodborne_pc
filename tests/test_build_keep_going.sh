#!/usr/bin/env bash
# build.sh --test-keep-going's runner (the block between build.sh's "--- test runner" markers)
# with fake tests: every test runs after a failure, each exit is in out/test-results.txt, the
# summary counts run/passed/failed/skipped and lists failures, the exit is 1 on any failure, 3 when
# something was skipped (NOT A FULL GATE) and 0 only when all ran and passed, a failed test's outputs are not checked again, a wrong output count is a failure,
# BB_TEST_SKIP skips (listed), and build.sh refuses BB_TEST_SKIP outside --test-keep-going.
set -euo pipefail
cd "$(dirname "$0")/.."
runner=$(mktemp); dir=$(mktemp -d); trap 'rm -rf "$runner" "$dir"' EXIT
sed -n '/# --- test runner (tests\/test_build_test_one.sh and test_build_keep_going.sh source this block) ---/,/# --- end test runner ---/p' \
    build.sh > "$runner"
[[ $(wc -l < "$runner") -gt 20 ]] || { echo "build.sh test runner block not found" >&2; exit 1; }
fail() { echo "build.sh --test-keep-going test FAILED: $*" >&2; exit 1; }
# run SKIP STATUS_A FILES STATUS_C: tests a (exit STATUS_A), b (writes FILES outputs, then
# validated by spirv-val, count checked against 2) and c (exit STATUS_C), under --test-keep-going.
run() {
    (cd "$dir"; rm -rf out; mkdir -p out/gpu out/b
     dry='' one='' keep=1 one_ran=0 one_failed=0 one_last=0 one_seen=() keep_ran=0 keep_skipped=() keep_failed=()
     BB_TEST_SKIP=$1
     : > out/test-results.txt
     sign_darwin() { :; }
     spirv-val() { echo "$*" >> "$dir/spirv-calls"; }
     : > "$dir/spirv-calls"
     source "$runner"
     printf '#!/bin/sh\nexit %d\n' "$2" > out/gpu/a-test
     printf '#!/bin/sh\nfor i in $(seq %d); do : > "$1/$i.spv"; done\n' "$3" > out/gpu/b-test
     printf '#!/bin/sh\nexit %d\n' "$4" > out/gpu/c-test
     chmod +x out/gpu/*-test
     t out/gpu/a-test
     t out/gpu/b-test out/b
     b_spv=(out/b/*.spv)
     outputs_ok ${#b_spv[@]} 2 "b-test wrote ${#b_spv[@]} SPIR-V files, expected 2"
     for spv in "${b_spv[@]}"; do t spirv-val "$spv"; done
     t out/gpu/c-test
     echo "results: $(tr '\n' '|' < out/test-results.txt)"
     echo "spirv-val calls: $(wc -l < "$dir/spirv-calls" | tr -d ' ')"
     test_keep_going_summary) 2>&1
}
set +e
out=$(run '' 0 2 0); status=$?
set -e
((status == 0)) && [[ $out == *'test-keep-going: 5 run, 5 passed, 0 failed, 0 skipped'* &&
    $out == *'spirv-val calls: 2'* ]] || fail "all pass (exit $status): $out"
echo "PASS all pass: 5 run (3 tests + 2 spirv-val), exit 0"
set +e
out=$(run '' 23 2 4); status=$?
set -e
((status == 1)) && [[ $out == *'test-keep-going: FAIL exit 23: out/gpu/a-test'* &&
    $out == *'test-keep-going: FAIL exit 4: out/gpu/c-test'* &&
    $out == *'5 run, 3 passed, 2 failed, 0 skipped'* &&
    $out == *'  FAILED: exit 23: out/gpu/a-test'* && $out == *'  FAILED: exit 4: out/gpu/c-test'* &&
    $out == *'results: exit 23: out/gpu/a-test|exit 0: out/gpu/b-test out/b|'* ]] ||
    fail "two failures, run on (exit $status): $out"
echo "PASS a failure does not stop the run: c ran after a; both listed; exit 1"
set +e
out=$(run '' 0 1 0); status=$?
set -e
((status == 1)) && [[ $out == *'FAILED: output check: b-test wrote 1 SPIR-V files, expected 2'* &&
    $out == *'spirv-val calls: 1'* && $out == *'5 run, 4 passed, 1 failed'* ]] ||
    fail "wrong output count (exit $status): $out"
echo "PASS a wrong output count is a failure (its outputs still validated); exit 1"
set +e
out=$(cd "$dir"; rm -rf out; mkdir -p out/gpu out/b
      dry='' one='' keep=1 one_ran=0 one_failed=0 one_last=0 one_seen=() keep_ran=0 keep_skipped=() keep_failed=()
      BB_TEST_SKIP=''; : > out/test-results.txt; sign_darwin() { :; }
      spirv-val() { echo "$*" >> "$dir/spirv-calls"; }; : > "$dir/spirv-calls"; source "$runner"
      printf '#!/bin/sh\nexit 23\n' > out/gpu/b-test; chmod +x out/gpu/b-test
      t out/gpu/b-test out/b; b_spv=(out/b/*.spv)
      outputs_ok ${#b_spv[@]} 2 "b-test wrote ${#b_spv[@]} SPIR-V files, expected 2"
      for spv in "${b_spv[@]}"; do t spirv-val "$spv"; done
      echo "spirv-val calls: $(wc -l < "$dir/spirv-calls" | tr -d ' ')"; test_keep_going_summary 2>&1); status=$?
set -e
((status == 1)) && [[ $out == *'1 run, 0 passed, 1 failed'* && $out != *'output check'* && $out == *'spirv-val calls: 0'* ]] ||
    fail "failed producer's outputs checked (exit $status): $out"
echo "PASS a failed test's outputs are not checked or validated (1 failure, not 2)"
set +e
out=$(run c-test 0 2 0); status=$?
set -e
((status == 3)) && [[ $out == *'4 run, 4 passed, 0 failed, 1 skipped'* && $out == *'  skipped: out/gpu/c-test'* &&
    $out == *'skipped (BB_TEST_SKIP): out/gpu/c-test'* && $out == *'NOT A FULL GATE: 1 skipped (exit 3)'* ]] ||
    fail "BB_TEST_SKIP (exit $status): $out"
echo "PASS BB_TEST_SKIP: skipped and listed; exit 3, NOT A FULL GATE (never 0)"
set +e
out=$(run c-test 23 2 0); status=$?
set -e
((status == 1)) && [[ $out == *'1 skipped'* && $out == *'FAILED: exit 23: out/gpu/a-test'* ]] ||
    fail "a failure with a skip (exit $status): $out"
echo "PASS a failure and a skip: exit 1 (the failure decides)"
set +e
out=$(BB_TEST_SKIP=x bash build.sh --test 2>&1 < /dev/null); status=$?  # refused before any slot or build
set -e
((status == 2)) && [[ $out == *'BB_TEST_SKIP is for build.sh --test-keep-going only'* ]] ||
    fail "BB_TEST_SKIP accepted outside --test-keep-going (exit $status): $out"
echo "PASS BB_TEST_SKIP outside --test-keep-going: refused (exit 2)"
