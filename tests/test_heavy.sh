#!/usr/bin/env bash
# tools/macos/heavy.sh in its own lock directory with one build slot and two playtest slots: a
# second build job waits for the first, a priority build does not, the playtest pool is separate,
# a killed holder (and its children) frees the slot, a nested call runs directly, and bad usage is
# an error.
# Hermetic: its own lock directory, slots and settings; nothing from the job running it (a full
# --test holds a real slot) or from real heavy.sh jobs is seen, paused or resumed.
set -euo pipefail
if [[ -z ${BB_HEAVY_TEST_INNER:-} ]]; then
    # A real perf run's --cpu-quiet stops this whole test with the job running it, which breaks its
    # timing (a pause looks like a job that never ran). A sampler sees such a stop as a gap; a failed
    # run with one is rerun once, and both results are said.
    self=$(cd "$(dirname "$0")" && pwd)/$(basename "$0")
    for attempt in 1 2; do
        gaps=$(mktemp)
        ( last=$EPOCHREALTIME
          while :; do sleep 0.2; now=$EPOCHREALTIME
              awk -v a="$last" -v b="$now" 'BEGIN { if (b - a > 1.5) printf "%.1f\n", b - a }' >> "$gaps"
              last=$now; done ) &
        sampler=$!
        status=0
        BB_HEAVY_TEST_INNER=1 bash "$self" || status=$?
        kill "$sampler"; wait "$sampler" 2>/dev/null || true
        stopped=$(awk '{ s += $1 } END { printf "%.0f", s }' "$gaps"); rm -f "$gaps"
        if (( status == 0 )); then
            (( attempt == 1 )) || echo "heavy test: PASS on the rerun (the first run failed after an outside pause)" >&2
            exit 0
        fi
        if (( attempt == 2 || stopped == 0 )); then exit "$status"; fi
        echo "heavy test: FAILED (exit $status) after an outside pause of ${stopped} s (a real perf run's" \
            "CPU quiet stops this test); rerunning once" >&2
    done
fi
cd "$(dirname "$0")/.."
# Nothing inherited: the slot and settings of the job running this test, its parallelism hints.
unset ${!BB_HEAVY_@} BB_BUILD_JOBS CMAKE_BUILD_PARALLEL_LEVEL MAKEFLAGS
heavy=tools/macos/heavy.sh
dir=$(mktemp -d)
trap 'rm -rf "$dir"; kill $(jobs -p) 2>/dev/null || true' EXIT
# The user counts as active unless a case says otherwise (the real idle time varies).
export BB_HEAVY_DIR=$dir BB_HEAVY_BUILD_SLOTS=1 BB_HEAVY_PLAYTEST_SLOTS=2 BB_HEAVY_QUICK_SLOTS=2 BB_HEAVY_SWEEP_SLOTS=1 BB_HEAVY_IDLE_SECONDS=0
# The slot tests below run several jobs from this one checkout; the worktree rule has its own
# section (in a scratch repository) near the end.
export BB_HEAVY_SHARED_TREE=1
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
BB_HEAVY_EXEC=1 "$heavy" --pool playtest G1 -- sleep 30 &
g1=$!
BB_HEAVY_EXEC=1 "$heavy" --pool playtest G2 -- sleep 30 &
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
BB_HEAVY_EXEC=1 "$heavy" A2 -- sleep 30 &
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
BB_HEAVY_EXEC=1 "$heavy" C -- sleep 30 &
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
BB_HEAVY_EXEC=1 "$heavy" LONG -- sleep 30 &
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
BB_HEAVY_EXEC=1 "$heavy" LONG2 -- sleep 30 &
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

# --cpu-quiet (perf runs): running build/quick/sweep jobs are paused (whole tree) and resumed after,
# a new one waits, a quick job's time limit pauses too, the pause is bounded, and the flag needs
# --gpu exclusive outside any slot. A paused job says so at its end, a failed one that a wall-clock
# timeout may have fired on resume, and its end event carries paused_s and paused_by.
ticks() { wc -l < "$1" | tr -d ' '; }
"$heavy" B10 -- sh -c "while :; do echo t >> '$dir/b10'; sleep 0.2; done" &
b10=$!
for _ in $(seq 400); do  # Q2's orphaned sleep 30 (killed supervisor) may still hold a quick slot
    [[ $("$heavy" status | grep -c '^quick[01]: free') == 2 ]] && break; sleep 0.1
done
BB_HEAVY_QUICK_LIMIT=4 "$heavy" --pool quick Q10 -- sh -c 'sleep 3' > /dev/null 2> "$dir/q10.err" &
q10=$!
"$heavy" --pool quick Q11 -- sh -c 'sleep 3; exit 3' > /dev/null 2> "$dir/q11.err" &
q11=$!
for _ in $(seq 50); do [[ -s $dir/b10 ]] && break; sleep 0.1; done
for _ in $(seq 100); do
    held=$("$heavy" status)
    [[ $held == *'held by Q10 '* && $held == *'held by Q11 '* ]] && break; sleep 0.1
done
[[ $held == *'held by Q10 '* && $held == *'held by Q11 '* ]] || fail "Q10/Q11 never started: $held"
"$heavy" --pool playtest --gpu exclusive --cpu-quiet P10 -- sh -c "sleep 0.5; a=\$(wc -l < '$dir/b10'); sleep 2.5; b=\$(wc -l < '$dir/b10'); echo \$a \$b > '$dir/p10'" 2> "$dir/p10.err" &
p10=$!
sleep 1
( "$heavy" --pool sweep B11 -- sh -c 'date +%s' > "$dir/b11" 2>/dev/null ) &  # its slot is free
b11=$!
wait "$p10"
p10_done=$(date +%s)
read -r a b < "$dir/p10"
(( a == b )) || fail "a build job kept running during a CPU-quiet perf run ($a -> $b ticks)"
grep -q 'P10: CPU quiet: paused B10' "$dir/p10.err" || fail "pause not reported: $(cat "$dir/p10.err")"
grep -q 'P10: CPU quiet: paused Q10' "$dir/p10.err" || fail "quick job pause not reported"
before=$(ticks "$dir/b10"); sleep 2
(( $(ticks "$dir/b10") > before )) || fail "a paused build job was not resumed"
grep -q 'P10: CPU quiet over: resumed B10' "$dir/p10.err" || fail "resume not reported"
set +e; wait "$q10"; q10_status=$?; set -e
(( q10_status == 0 )) || fail "the quick job's time limit ran during the pause (exit $q10_status)"
set +e; wait "$q11"; q11_status=$?; set -e
(( q11_status == 3 )) || fail "a paused failing job's status changed (exit $q11_status)"
grep -Eq '^heavy: Q10: PAUSED [0-9]+ s for perf runs \(P10\); ended with exit 0$' "$dir/q10.err" ||
    fail "paused job's end line: $(cat "$dir/q10.err")"
grep -Eq '^heavy: Q11: PAUSED [0-9]+ s for perf runs \(P10\); ended with exit 3: a wall-clock timeout in the job may have fired on resume' "$dir/q11.err" ||
    fail "paused failed job's end line: $(cat "$dir/q11.err")"
python3 - "$dir/history.jsonl" "$dir" <<'PY' || fail "paused_s/paused_by in the end events, pause files consumed"
import json, os, sys
events = [json.loads(line) for line in open(sys.argv[1])]
ends = {e['label']: e for e in events if e.get('event') == 'end'}
pids = {e['label']: e['pid'] for e in events if e.get('event') == 'start'}
for label, status in (('Q10', 0), ('Q11', 3)):
    e = ends[label]
    assert e['status'] == status and e['paused_by'] == ['P10'] and 1.5 <= e['paused_s'] <= 6, e
    assert not os.path.exists(os.path.join(sys.argv[2], f'paused-{pids[label]}.jsonl')), label
PY
wait "$b11"
(( $(cat "$dir/b11") >= p10_done - 1 )) || fail "a build job started during a CPU-quiet perf run"
kill "$b10" 2>/dev/null; wait "$b10" 2>/dev/null || true; pkill -f "$dir/b10" 2>/dev/null || true
rm -f "$dir/b10"
"$heavy" B12 -- sh -c "while :; do echo t >> '$dir/b12'; sleep 0.2; done" &
b12=$!
for _ in $(seq 50); do [[ -s $dir/b12 ]] && break; sleep 0.1; done
BB_HEAVY_QUIET_MAX=2 "$heavy" --pool playtest --gpu exclusive --cpu-quiet P11 -- sleep 5 2> "$dir/p11.err" &
p11=$!
sleep 3.5
before=$(ticks "$dir/b12"); sleep 1
(( $(ticks "$dir/b12") > before )) || fail "the CPU-quiet bound did not resume the build job"
grep -q 'CPU QUIET BOUND REACHED' "$dir/p11.err" || fail "bound not reported loudly"
wait "$p11"
kill "$b12" 2>/dev/null; wait "$b12" 2>/dev/null || true; pkill -f "$dir/b12" 2>/dev/null || true
# A process another pause already stopped is left to it: a CPU-quiet run neither counts nor resumes it
# (a real perf run pausing a full --test, then resuming everything, must not resume what this
# test's own perf run paused).
"$heavy" B13 -- sh -c "while :; do echo t >> '$dir/b13'; sleep 0.2; done" &
b13=$!
for _ in $(seq 50); do [[ -s $dir/b13 ]] && break; sleep 0.1; done
b13_loop=$(pgrep -f "^sh -c while :; do echo t >> '$dir/b13'" || true)
[[ -n $b13_loop ]] || fail "B13's loop not found"
kill -STOP "$b13_loop"
"$heavy" --pool playtest --gpu exclusive --cpu-quiet P12 -- sleep 1 2> "$dir/p12.err"
sleep 0.5
[[ $(ps -o stat= -p "$b13_loop") == T* ]] || fail "a CPU-quiet run resumed a process another pause had stopped"
grep -q 'P12: CPU quiet: paused B13 (build0, [0-9]* processes; 1 already stopped by another pause, left to it)' "$dir/p12.err" ||
    fail "already-stopped process not reported: $(cat "$dir/p12.err")"
kill -CONT "$b13_loop"
before=$(ticks "$dir/b13"); sleep 1
(( $(ticks "$dir/b13") > before )) || fail "B13 did not run after its own pauser resumed it"
kill "$b13" 2>/dev/null; wait "$b13" 2>/dev/null || true; pkill -f "$dir/b13" 2>/dev/null || true
# A job that ends the moment it is resumed (its sleep ran out during the pause) still reads the
# whole pause: the end is written before SIGCONT, and no pause file is left behind.
"$heavy" --pool quick Q14 -- sleep 0.3 2> "$dir/q14.err" &
q14=$!
until_held 'quick[01]' Q14
"$heavy" --pool playtest --gpu exclusive --cpu-quiet P13 -- sleep 2 2> /dev/null
wait "$q14" || fail "Q14 failed"
sleep 0.5
python3 - "$dir/history.jsonl" "$dir" <<'PY' || fail "a job ending at once after its resume lost its pause: $(cat "$dir/q14.err")"
import json, os, sys
events = [json.loads(line) for line in open(sys.argv[1])]
end = [e for e in events if e.get('event') == 'end' and e['label'] == 'Q14'][-1]
pid = [e for e in events if e.get('event') == 'start' and e['label'] == 'Q14'][-1]['pid']
assert end['paused_by'] == ['P13'] and end['paused_s'] >= 1.5, end
assert not os.path.exists(os.path.join(sys.argv[2], f'paused-{pid}.jsonl')), 'pause file left behind'
PY
if "$heavy" --cpu-quiet K1 -- true 2>/dev/null; then fail "--cpu-quiet without --gpu exclusive accepted"; fi
if "$heavy" K2 -- "$heavy" --pool playtest --gpu exclusive --cpu-quiet K3 -- true 2>/dev/null; then
    fail "--cpu-quiet inside a slot accepted"
fi

# Job records: a start event with the command and the directory; an end event for every pool with
# the exit status, or the signal that killed it (SIGTERM to heavy.sh is forwarded to the command);
# a job whose heavy.sh was SIGKILLed while its command runs on is shown as ORPHANED.
"$heavy" R1 -- sh -c 'exit 7' 2>/dev/null || true
"$heavy" R2 -- sleep 30 &
r2=$!
until_held build0 R2
kill -TERM "$r2"; wait "$r2" 2>/dev/null || true
for _ in $(seq 50); do "$heavy" status | grep -q '^build0: free' && break; sleep 0.1; done
"$heavy" status | grep -q '^build0: free' || fail "SIGTERM to heavy.sh did not end its command"
"$heavy" R3 -- sleep 30 &
r3=$!
until_held build0 R3
kill -9 "$r3"; wait "$r3" 2>/dev/null || true
"$heavy" status | grep -q '^build0: held by R3 .*ORPHANED' || fail "orphaned command not shown: $("$heavy" status | grep build0)"
kill "$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1]))["command_pid"])' "$dir/build0")"
for _ in $(seq 50); do "$heavy" status | grep -q '^build0: free' && break; sleep 0.1; done
python3 - "$dir/history.jsonl" "$PWD" <<'PY' || fail "job records in history.jsonl"
import json, sys
lines = [json.loads(l) for l in open(sys.argv[1])]
start = {l['label']: l for l in lines if l['event'] == 'start'}
end = {l['label']: l for l in lines if l['event'] == 'end'}
assert start['R1']['command'].endswith("sh -c exit 7") and start['R1']['cwd'] == sys.argv[2], start['R1']
assert end['R1']['status'] == 7 and end['R1']['pool'] == 'build' and 'seconds' in end['R1'], end['R1']
assert end['R2']['signal'] == 'SIGTERM' and end['R2']['status'] == 128 + 15, end['R2']
assert 'R3' not in end  # its heavy.sh was SIGKILLed: no end event, and status said ORPHANED
assert start['G1']['end'].startswith('recorded by the caller'), start['G1']
PY

# One build/test job per worktree, and a job whose worktree changes during the run is INVALID
# (exit 125 instead of 0, said on stderr and in its end event). Games may share a worktree.
repo=$dir/repo
git init -q "$repo" && git -C "$repo" config user.email t@t && git -C "$repo" config user.name t
echo one > "$repo/f" && git -C "$repo" add f && git -C "$repo" commit -qm one
heavy_abs=$PWD/$heavy
(
    cd "$repo"
    heavy=$heavy_abs  # until_held calls "$heavy"
    unset BB_HEAVY_SHARED_TREE
    "$heavy_abs" W1 -- true || fail "an untouched worktree job failed"
    set +e
    "$heavy_abs" W2 -- sh -c 'echo two > f; git commit -qam two' 2> "$dir/w2.err"; w2=$?
    "$heavy_abs" W3 -- sh -c 'echo edited >> f' 2> "$dir/w3.err"; w3=$?
    git checkout -q f
    "$heavy_abs" W4 -- sh -c 'echo three > f; git commit -qam three; exit 3' 2>/dev/null; w4=$?
    set -e
    (( w2 == 125 )) || fail "a job that moved HEAD exited $w2, expected 125"
    grep -q 'W2: INVALID: HEAD of .* changed during the run' "$dir/w2.err" || fail "HEAD change not reported: $(cat "$dir/w2.err")"
    (( w3 == 125 )) || fail "a job that edited tracked files exited $w3, expected 125"
    grep -q 'W3: INVALID: tracked files in .* were edited during the run' "$dir/w3.err" || fail "edit not reported"
    (( w4 == 3 )) || fail "a failing INVALID job must keep its own exit status ($w4)"
    "$heavy_abs" --pool sweep W5 -- sleep 30 &
    w5=$!
    until_held sweep0 W5
    set +e; out=$("$heavy_abs" --pool quick W6 -- true 2>&1); w6=$?; set -e
    (( w6 == 2 )) && [[ $out == *"is busy with W5"* ]] || fail "a second job in a busy worktree was not refused ($w6: $out)"
    BB_HEAVY_SHARED_TREE=1 "$heavy_abs" --pool quick W7 -- true || fail "BB_HEAVY_SHARED_TREE=1 job refused"
    BB_HEAVY_EXEC=1 "$heavy_abs" --pool playtest W8 -- true || fail "a game was refused in a busy worktree"
    kill -TERM "$w5"; wait "$w5" 2>/dev/null || true
    "$heavy_abs" W9 -- true || fail "the worktree was not freed after its job ended"
)
python3 - "$dir/history.jsonl" <<'PY' || fail "worktree fields in history.jsonl"
import json, sys
lines = [json.loads(l) for l in open(sys.argv[1])]
start = {l['label']: l for l in lines if l['event'] == 'start'}
end = {l['label']: l for l in lines if l['event'] == 'end'}
assert start['W1']['worktree'].endswith('/repo') and len(start['W1']['head']) == 40 and start['W1']['dirty'] is False, start['W1']
assert 'invalid' not in end['W1'] and end['W1']['status'] == 0, end['W1']
assert end['W2']['status'] == 125 and 'HEAD' in end['W2']['invalid'], end['W2']
assert end['W4']['status'] == 3 and 'HEAD' in end['W4']['invalid'], end['W4']
PY

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
echo "heavy.sh: history log, build/playtest/quick pools, quick hints and time limit, waiting, priority slot, GPU shared/exclusive lock with gate, start announcement, polite scheduling, idle-aware admission, CPU-quiet pause (leaving another pause's processes stopped; named at the job's end), job records (command, directory, end status/signal, orphans), one job per worktree and INVALID on a changed tree, sweep lane, release on SIGKILL, nesting, exit status, usage errors PASS"
