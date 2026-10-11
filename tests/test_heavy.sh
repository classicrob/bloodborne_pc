#!/usr/bin/env bash
# tools/macos/heavy.sh in its own lock directory with one build slot and two playtest slots: a
# second build job waits for the first, a priority build does not, the playtest pool is separate,
# a killed holder (and its children) frees the slot, a nested call runs directly, and bad usage is
# an error.
set -euo pipefail
cd "$(dirname "$0")/.."
heavy=tools/macos/heavy.sh
dir=$(mktemp -d)
trap 'rm -rf "$dir"; kill $(jobs -p) 2>/dev/null || true' EXIT
export BB_HEAVY_DIR=$dir BB_HEAVY_BUILD_SLOTS=1 BB_HEAVY_PLAYTEST_SLOTS=2
unset BB_HEAVY_HELD
fail() { echo "heavy test FAILED: $*" >&2; exit 1; }
until_held() { for _ in $(seq 100); do "$heavy" status | grep -q "$1: held by $2" && return; sleep 0.1; done; fail "$1 never held by $2"; }

# A holds the only ordinary slot (its child sleep inherits the lock).
"$heavy" A -- sh -c 'sleep 3' &
holder=$!
until_held build0 A
# B must wait until A ends.
start=$(date +%s)
"$heavy" B -- true 2>/dev/null
waited=$(( $(date +%s) - start ))
(( waited >= 1 )) || fail "B ran while A held the only slot (waited ${waited}s)"
wait "$holder"

# Game runs have their own pool: two run while the build slot is busy, a third waits.
"$heavy" --pool playtest G1 -- sleep 30 &
g1=$!
"$heavy" --pool playtest G2 -- sleep 30 &
g2=$!
until_held playtest0 G; until_held playtest1 G
start=$(date +%s)
( "$heavy" --pool playtest G3 -- true 2>/dev/null; echo done > "$dir/g3" ) &
sleep 2
[[ ! -e $dir/g3 ]] || fail "third game ran with two playtest slots held"
kill -9 "$g1"; wait "$g1" 2>/dev/null || true
for _ in $(seq 50); do [[ -e $dir/g3 ]] && break; sleep 0.1; done
[[ -e $dir/g3 ]] || fail "third game did not get the freed playtest slot"
kill -9 "$g2"; wait "$g2" 2>/dev/null || true

# A priority job runs while an ordinary job holds the ordinary slot.
"$heavy" C -- sleep 30 &
holder=$!
until_held build0 C
start=$(date +%s)
"$heavy" --priority P -- true
(( $(date +%s) - start < 2 )) || fail "priority job waited for the ordinary slot"
# Ordinary jobs never take the priority slot.
"$heavy" status | grep -q '^build-priority: free' || fail "priority slot not free after P"

# Killing the holder frees the slot: no lease, no cleanup.
kill -9 "$holder"; wait "$holder" 2>/dev/null || true
for _ in $(seq 50); do "$heavy" status | grep -q '^build0: free' && break; sleep 0.1; done
"$heavy" status | grep -q '^build0: free' || fail "build0 still held after SIGKILL of its holder"

# Inside a slot, a nested call runs directly (build.sh under heavy.sh).
"$heavy" D -- sh -c "$heavy E -- true" || fail "nested heavy.sh blocked or failed"

# The command's exit status is the job's.
set +e; "$heavy" F -- sh -c 'exit 7'; status=$?; set -e
(( status == 7 )) || fail "exit status $status, expected 7"
# Bad usage is an error, not a silent no-op.
if "$heavy" G true 2>/dev/null; then fail "missing -- accepted"; fi
if BB_HEAVY_BUILD_SLOTS=0 "$heavy" H -- true 2>/dev/null; then fail "zero slots accepted"; fi
if "$heavy" --pool playtest --priority I -- true 2>/dev/null; then fail "priority accepted for the playtest pool"; fi
if "$heavy" --pool other J -- true 2>/dev/null; then fail "unknown pool accepted"; fi
echo "heavy.sh: build/playtest pools, waiting, priority slot, release on SIGKILL, nesting, exit status, usage errors PASS"
