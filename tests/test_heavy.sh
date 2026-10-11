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
# The user counts as active unless a case says otherwise (the real idle time varies).
export BB_HEAVY_DIR=$dir BB_HEAVY_BUILD_SLOTS=1 BB_HEAVY_PLAYTEST_SLOTS=2 BB_HEAVY_QUICK_SLOTS=2 BB_HEAVY_SWEEP_SLOTS=1 BB_HEAVY_IDLE_SECONDS=0
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

# Quick jobs: their own two slots, the -j4 hint, and a hard time limit for the whole group.
"$heavy" A2 -- sleep 30 &
busy=$!
until_held build0 A2
out=$("$heavy" --pool quick Q1 -- sh -c 'echo "$BB_BUILD_JOBS $CMAKE_BUILD_PARALLEL_LEVEL $MAKEFLAGS"')
[[ $out == "4 4 -j4" ]] || fail "quick job parallelism hint: '$out'"
"$heavy" --pool quick Q2 -- sleep 30 &
q2=$!
until_held quick0 Q2
start=$(date +%s)
"$heavy" --pool quick Q3 -- true
(( $(date +%s) - start < 10 )) || fail "second quick slot not available"  # the holder sleeps 30 s
marker=bbheavytest$$
set +e
BB_HEAVY_QUICK_WARN=1 BB_HEAVY_QUICK_LIMIT=2 "$heavy" --pool quick Q4 -- \
    sh -c "exec -a $marker sleep 30 & exec -a $marker sleep 30" 2> "$dir/q4.err"
status=$?
set -e
(( status == 124 )) || fail "over-limit quick job exit status $status, expected 124"
grep -q 'WARNING: quick job Q4' "$dir/q4.err" || fail "no warning before the quick limit"
grep -q 'KILLED quick job Q4' "$dir/q4.err" || fail "quick limit kill not reported"
sleep 0.5
! pgrep -f "$marker" >/dev/null || fail "a child of the killed quick job survived"
"$heavy" status | grep -qE '^quick[01]: free' || fail "quick slot not freed after the limit kill"
kill -9 "$q2" "$busy"; wait "$q2" "$busy" 2>/dev/null || true

# A priority job runs while an ordinary job holds the ordinary slot.
"$heavy" C -- sleep 30 &
holder=$!
until_held build0 C
start=$(date +%s)
"$heavy" --priority P -- true
(( $(date +%s) - start < 10 )) || fail "priority job waited for the ordinary slot"  # the holder sleeps 30 s
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
# The GPU lock: games share it; a timing job takes it exclusively after the running games end;
# a game started while that job waits starts only after it; a game under the job runs directly;
# exclusive inside a shared hold is an error.
export BB_HEAVY_PLAYTEST_SLOTS=3
"$heavy" --pool playtest G4 -- sleep 3 &
g4=$!
"$heavy" --pool playtest G5 -- sleep 3 &
g5=$!
for _ in $(seq 100); do "$heavy" status | grep -q '^gpu: shared' && break; sleep 0.1; done
"$heavy" status | grep -q '^gpu: shared' || fail "games did not hold the GPU shared"
start=$(date +%s)
( "$heavy" --pool quick --gpu exclusive T1 -- sh -c 'echo "$BB_HEAVY_GPU"; sleep 2' > "$dir/t1" ) &
t1=$!
sleep 1
( "$heavy" --pool playtest G6 -- sh -c 'date +%s' > "$dir/g6" ) &
g6=$!
wait "$t1"
t1_done=$(date +%s)
[[ $(cat "$dir/t1") == exclusive ]] || fail "timing job did not hold the GPU exclusive"
(( t1_done - start >= 4 )) || fail "exclusive GPU job ran while games held the GPU ($((t1_done - start)) s)"
wait "$g6"
(( $(cat "$dir/g6") >= t1_done - 1 )) || fail "a game started while an exclusive GPU job waited or ran"
wait "$g4" "$g5"
out=$("$heavy" --pool quick --gpu exclusive T2 -- "$heavy" --pool playtest G7 -- sh -c 'echo "$BB_HEAVY_GPU"')
[[ $out == exclusive ]] || fail "a game under an exclusive GPU job did not run directly ('$out')"
if "$heavy" --pool playtest G8 -- "$heavy" --gpu exclusive T3 -- true 2>/dev/null; then
    fail "exclusive GPU inside a shared hold accepted"
fi
"$heavy" status | grep -q '^gpu: free' || fail "GPU lock not free after the jobs"
export BB_HEAVY_PLAYTEST_SLOTS=2

# BB_HEAVY_ANNOUNCE (play_route.py): "running" on stderr when the command starts, after any wait,
# also for a nested call; nothing without it.
err=$(BB_HEAVY_ANNOUNCE=1 "$heavy" --pool playtest G9 -- true 2>&1 >/dev/null)
[[ $err == *'heavy: G9: running'* ]] || fail "no running announcement ('$err')"
err=$(BB_HEAVY_ANNOUNCE=1 "$heavy" --pool quick --gpu exclusive T4 -- "$heavy" --pool playtest G10 -- true 2>&1 >/dev/null)
[[ $err == *'heavy: G10: running'* ]] || fail "no running announcement for a nested call ('$err')"
err=$("$heavy" --pool playtest G11 -- true 2>&1 >/dev/null)
[[ $err != *running* ]] || fail "announced without BB_HEAVY_ANNOUNCE ('$err')"

# Scheduling: builds/tests (build, quick) as background QoS (scheduler priority 4) with compile
# parallelism capped at 8, the priority lane (P0 previews) at nice +5 with a utility clamp and the cap, correctness games (playtest) at nice +5 with a utility clamp (priority
# 20), exclusive-GPU jobs unchanged; applied once; BB_HEAVY_POLITE=0 turns it off. Niceness is
# relative to this shell's. The QoS checks need a normal-priority shell: under build.sh --test
# this test itself runs as background QoS, and they are reported as not checked.
base=$(ps -o nice= -p $$ | tr -d ' ') base_pri=$(ps -o pri= -p $$ | tr -d ' ')
want() { local n=$((base + $1)); ((n > 20)) && n=20; echo "$n"; }
probe() { "$heavy" "$@" -- sh -c 'echo "$(ps -o pri= -p $$) $(ps -o nice= -p $$) $BB_BUILD_JOBS"' 2>/dev/null; }
read -r pri nic jobs <<< "$(probe N1)"
(( jobs == 8 )) || fail "build job parallelism not capped at 8 ('$jobs')"
read -r _ _ jobs <<< "$(BB_BUILD_JOBS=4 probe --pool quick N2)"
(( jobs == 4 )) || fail "a lower parallelism hint was raised ('$jobs')"
read -r rpri rnic rjobs <<< "$(probe --priority NP)"
[[ $rnic == $(want 5) && $rjobs == 8 ]] || fail "priority-lane job not at nice +5 with 8 jobs ('$rnic' '$rjobs')"
read -r gpri gnic _ <<< "$(probe --pool playtest N3)"
[[ $gnic == $(want 5) ]] || fail "game not at nice +5"
read -r xpri xnic _ <<< "$(probe --pool playtest --gpu exclusive N4)"
[[ $xnic == "$base" ]] || fail "exclusive-GPU job was deprioritized"
[[ $("$heavy" --pool playtest N5 -- "$heavy" --pool playtest N6 -- sh -c 'ps -o nice= -p $$' 2>/dev/null | tr -d ' ') == $(want 5) ]] ||
    fail "nested call added to the niceness"
read -r ppri pnic _ <<< "$(BB_HEAVY_POLITE=0 probe --pool playtest N7)"
[[ $pnic == "$base" ]] || fail "BB_HEAVY_POLITE=0 did not turn it off"
if (( base_pri >= 20 )); then
    (( pri == 4 )) || fail "build job not background QoS (priority $pri)"
    (( gpri == 20 )) || fail "game not utility QoS (priority $gpri)"
    (( rpri == 20 )) || fail "priority-lane job not utility QoS (priority $rpri)"
    (( xpri == base_pri )) || fail "exclusive-GPU job QoS changed (priority $xpri)"
    qos=checked
else
    qos="not checked: this test runs at scheduler priority $base_pri"
fi
echo "heavy.sh scheduling: niceness and parallelism checked; QoS $qos"

# The sweep lane: a sweep runs while a long build job holds the build slot; a sweep started
# inside a build job runs in that slot and says so.
"$heavy" LONG -- sleep 30 &
long=$!
until_held build0 LONG
start=$(date +%s)
"$heavy" --pool sweep SW1 -- true
(( $(date +%s) - start < 10 )) || fail "sweep waited for the build slot"  # the holder sleeps 30 s
"$heavy" status | grep -q '^sweep0: free' || fail "sweep slot not listed or not freed"
kill -9 "$long"; wait "$long" 2>/dev/null || true
err=$("$heavy" B9 -- "$heavy" --pool sweep SW2 -- true 2>&1 >/dev/null)
[[ $err == *'SW2: started inside build0'* ]] || fail "a sweep nested in a build job was not reported ('$err')"

# Idle-aware admission: while the user is idle (>= BB_HEAVY_IDLE_MINUTES), a second build job
# runs (build-idle) at nice +5 with a utility clamp and says why; while active it waits, and a
# job runs as background QoS and says why.
"$heavy" LONG2 -- sleep 30 &
long=$!
until_held build0 LONG2
start=$(date +%s)
out=$(BB_HEAVY_IDLE_SECONDS=4500 "$heavy" IDLE1 -- sh -c 'echo "$(ps -o pri= -p $$) $(ps -o nice= -p $$) $BB_BUILD_JOBS"' 2>"$dir/idle.err")
(( $(date +%s) - start < 10 )) || fail "idle user: a second build job waited for build0"  # the holder sleeps 30 s
read -r ipri inic ijobs <<< "$out"
[[ $inic == $(want 5) && $ijobs == 8 ]] || fail "idle admission not nice +5 with 8 jobs ('$out')"
(( base_pri < 20 || ipri == 20 )) || fail "idle admission not utility QoS (priority $ipri)"
grep -q 'IDLE1: user idle 75 min: nice 5, utility QoS, 8 jobs' "$dir/idle.err" || fail "idle decision not logged: $(cat "$dir/idle.err")"
( BB_HEAVY_IDLE_SECONDS=0 "$heavy" ACTIVE1 -- true 2>/dev/null; echo done > "$dir/active1" ) &
sleep 3
[[ ! -e $dir/active1 ]] || fail "active user: a second build job ran while build0 was held"
kill -9 "$long"; wait "$long" 2>/dev/null || true
for _ in $(seq 100); do [[ -e $dir/active1 ]] && break; sleep 0.1; done
[[ -e $dir/active1 ]] || fail "active user: the waiting build job never ran"
err=$(BB_HEAVY_IDLE_SECONDS=120 "$heavy" ACTIVE2 -- true 2>&1 >/dev/null)
[[ $err == *'ACTIVE2: user active (idle 2 min): background QoS, 8 jobs'* ]] || fail "active decision not logged ('$err')"

# Bad usage is an error, not a silent no-op.
if "$heavy" G true 2>/dev/null; then fail "missing -- accepted"; fi
if BB_HEAVY_BUILD_SLOTS=0 "$heavy" H -- true 2>/dev/null; then fail "zero slots accepted"; fi
if "$heavy" --pool playtest --priority I -- true 2>/dev/null; then fail "priority accepted for the playtest pool"; fi
if "$heavy" --pool other J -- true 2>/dev/null; then fail "unknown pool accepted"; fi
if "$heavy" --gpu some K -- true 2>/dev/null; then fail "unknown GPU mode accepted"; fi
# Every acquisition is in the history; quick jobs also record their end.
python3 - "$dir/history.jsonl" <<'PY' || fail "history.jsonl"
import json, sys
lines = [json.loads(l) for l in open(sys.argv[1])]
starts = [l for l in lines if l['event'] == 'start']
ends = [l for l in lines if l['event'] == 'end']
assert any(l['label'] == 'B' and l['waited_s'] >= 1 for l in starts), 'B waited'
assert all({'pool', 'slot', 'waited_s', 'load', 'held'} <= set(l) for l in starts)
assert any(l['label'] == 'Q4' and l['status'] == 124 for l in ends), 'Q4 killed'
PY
echo "heavy.sh: history log, build/playtest/quick pools, quick hints and time limit, waiting, priority slot, GPU shared/exclusive lock with gate, start announcement, polite scheduling, idle-aware admission, sweep lane, release on SIGKILL, nesting, exit status, usage errors PASS"
