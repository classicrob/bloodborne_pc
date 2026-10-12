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
# Robust under load and under a real perf run's pause (this whole test is then stopped): every wait
# is on a condition with a generous ceiling (60 s), "did not wait" is shown by the history (a start
# record lists the slots held at that moment; events are logged in the order they happen), and a
# perf-run stand-in keeps its quiet until the test releases it. No check depends on how long
# anything took. Every heavy.sh call's failure is a failure with a message.
wait_for() {  # wait_for MESSAGE COMMAND...: until COMMAND succeeds, at most 60 s
    local what=$1; shift
    for _ in $(seq 600); do "$@" && return 0; sleep 0.1; done
    fail "$what (not within 60 s)"
}
until_held() { wait_for "$1 never held by $2" held_by "$1" "$2"; }
held_by() { "$heavy" status | grep -q "$1: held by $2"; }
slot_free() { "$heavy" status | grep -q "^$1: free"; }
# order EVENT:LABEL...: each event is in the history, each after the one before (a quiet run's events
# carry its label as "by").
order() { python3 - "$dir/history.jsonl" "$@" <<'PY' || fail "history order: $*"
import json, sys
lines = [json.loads(l) for l in open(sys.argv[1])]
last = -1
for spec in sys.argv[2:]:
    event, label = spec.split(':', 1)
    found = [i for i, e in enumerate(lines) if e.get('event') == event and label in (e.get('label'), e.get('by'))]
    later = [i for i in found if i > last]
    assert later, f'{spec}: at lines {found}, none after line {last}'
    last = later[0]
PY
}
# held_at_start LABEL SLOT: LABEL's start record lists SLOT as held then (it ran beside SLOT's job).
held_at_start() { python3 - "$dir/history.jsonl" "$1" "$2" <<'PY' || fail "$1 did not start while $2 was held"
import json, sys
starts = [json.loads(l) for l in open(sys.argv[1])]
starts = [e for e in starts if e.get('event') == 'start' and e.get('label') == sys.argv[2]]
assert starts and sys.argv[3] in starts[-1]['held'], starts
PY
}
ticks() { wc -l < "$1" | tr -d ' '; }
grew() {  # grew FILE: FILE (a job's tick log) grows within 60 s
    local before; before=$(ticks "$1")
    for _ in $(seq 600); do (( $(ticks "$1") > before )) && return 0; sleep 0.1; done
    return 1
}
# A perf-run stand-in: waits (at most 60 s) for CONDITION, then for the test's release file.
hold() { echo "n=0; until $1; do n=\$((n + 1)); [ \$n -gt 600 ] && exit 9; sleep 0.1; done"; }

# A holds the only ordinary slot (its child sleep inherits the lock).
"$heavy" A -- sh -c 'sleep 3' &
holder=$!
until_held build0 A
# B must wait until A ends.
"$heavy" B -- true 2>/dev/null || fail "B failed"
wait "$holder" || fail "A failed"
order end:A start:B

# Game runs have their own pool: two run while the build slot is busy, a third waits.
BB_HEAVY_EXEC=1 "$heavy" --pool playtest G1 -- sleep 30 &
g1=$!
BB_HEAVY_EXEC=1 "$heavy" --pool playtest G2 -- sleep 30 &
g2=$!
until_held playtest0 G; until_held playtest1 G
( "$heavy" --pool playtest G3 -- true 2>/dev/null; echo done > "$dir/g3" ) &
sleep 2  # (a lower bound only: under load G3 just starts later)
[[ ! -e $dir/g3 ]] || fail "third game ran with two playtest slots held"
kill -9 "$g1"; wait "$g1" 2>/dev/null || true
wait_for "third game did not get the freed playtest slot" test -e "$dir/g3"
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
"$heavy" --pool quick Q3 -- true || fail "Q3 failed"
held_at_start Q3 quick0  # it ran beside Q2: the second quick slot
marker=bbheavytest$$
set +e
BB_HEAVY_QUICK_WARN=1 BB_HEAVY_QUICK_LIMIT=2 "$heavy" --pool quick Q4 -- \
    sh -c "exec -a $marker sleep 30 & exec -a $marker sleep 30" 2> "$dir/q4.err"
status=$?
set -e
(( status == 124 )) || fail "over-limit quick job exit status $status, expected 124"
grep -q 'WARNING: quick job Q4' "$dir/q4.err" || fail "no warning before the quick limit"
grep -q 'KILLED quick job Q4' "$dir/q4.err" || fail "quick limit kill not reported"
no_marker() { ! pgrep -f "$marker" > /dev/null; }
wait_for "a child of the killed quick job survived" no_marker
wait_for "quick slot not freed after the limit kill" slot_free 'quick[01]'
kill -9 "$q2" "$busy"; wait "$q2" "$busy" 2>/dev/null || true

# A priority job runs while an ordinary job holds the ordinary slot.
BB_HEAVY_EXEC=1 "$heavy" C -- sleep 30 &
holder=$!
until_held build0 C
"$heavy" --priority P -- true || fail "P failed"
held_at_start P build0  # it ran beside C
# Ordinary jobs never take the priority slot.
"$heavy" status | grep -q '^build-priority: free' || fail "priority slot not free after P"

# Killing the holder frees the slot: no lease, no cleanup.
kill -9 "$holder"; wait "$holder" 2>/dev/null || true
wait_for "build0 still held after SIGKILL of its holder" slot_free build0

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
gpu_shared() { "$heavy" status | grep -q '^gpu: shared'; }
wait_for "games did not hold the GPU shared" gpu_shared
( "$heavy" --pool quick --gpu exclusive T1 -- sh -c 'echo "$BB_HEAVY_GPU"; sleep 2' > "$dir/t1" ) &
t1=$!
# T1 holds the GPU gate (exclusive) while it waits for the games: a game asking now waits behind it.
gate_taken() { python3 -c '
import fcntl, os, sys
fd = os.open(sys.argv[1], os.O_RDONLY)
try:
    fcntl.flock(fd, fcntl.LOCK_SH | fcntl.LOCK_NB)
    sys.exit(1)
except BlockingIOError:
    sys.exit(0)' "$dir/gpu-gate" 2>/dev/null; }
wait_for "T1 never took the GPU gate" gate_taken
( "$heavy" --pool playtest G6 -- true ) &
g6=$!
wait "$t1" || fail "T1 failed"
[[ $(cat "$dir/t1") == exclusive ]] || fail "timing job did not hold the GPU exclusive"
wait "$g6" || fail "G6 failed"
wait "$g4" "$g5" || fail "G4/G5 failed"
order end:G4 gpu:T1  # the exclusive job got the GPU after the games ended
order end:G5 gpu:T1
order end:T1 gpu:G6  # a game asking while it waited got the GPU after it
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
"$heavy" --pool sweep SW1 -- true || fail "SW1 failed"
held_at_start SW1 build0  # it ran beside LONG
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
out=$(BB_HEAVY_IDLE_SECONDS=4500 "$heavy" IDLE1 -- sh -c 'echo "$(ps -o pri= -p $$) $(ps -o nice= -p $$) $BB_BUILD_JOBS"' 2>"$dir/idle.err") ||
    fail "IDLE1 failed: $(cat "$dir/idle.err")"
held_at_start IDLE1 build0  # idle user: a second build job ran beside LONG2
read -r ipri inic ijobs <<< "$out"
[[ $inic == $(want 5) && $ijobs == 8 ]] || fail "idle admission not nice +5 with 8 jobs ('$out')"
(( base_pri < 20 || ipri == 20 )) || fail "idle admission not utility QoS (priority $ipri)"
grep -q 'IDLE1: user idle 75 min: nice 5, utility QoS, 8 jobs' "$dir/idle.err" || fail "idle decision not logged: $(cat "$dir/idle.err")"
( BB_HEAVY_IDLE_SECONDS=0 "$heavy" ACTIVE1 -- true 2>/dev/null; echo done > "$dir/active1" ) &
sleep 3  # (a lower bound only)
[[ ! -e $dir/active1 ]] || fail "active user: a second build job ran while build0 was held"
kill -9 "$long"; wait "$long" 2>/dev/null || true
wait_for "active user: the waiting build job never ran" test -e "$dir/active1"
err=$(BB_HEAVY_IDLE_SECONDS=120 "$heavy" ACTIVE2 -- true 2>&1 >/dev/null)
[[ $err == *'ACTIVE2: user active (idle 2 min): background QoS, 8 jobs'* ]] || fail "active decision not logged ('$err')"

# --cpu-quiet (perf runs): running build/quick/sweep jobs are paused (whole tree) and resumed after,
# a new one waits, a quick job's time limit pauses too, the pause is bounded, and the flag needs
# --gpu exclusive outside any slot. A paused job says so at its end, a failed one that a wall-clock
# timeout may have fired on resume, and its end event carries paused_s and paused_by.
"$heavy" B10 -- sh -c "while :; do echo t >> '$dir/b10'; sleep 0.2; done" &
b10=$!
both_quick_free() { [[ $("$heavy" status | grep -c '^quick[01]: free') == 2 ]]; }
wait_for "quick slots not free (Q2's killed job)" both_quick_free
# Q10/Q11 wait for a go-file the test creates while they are stopped: they are paused for sure, and
# end as soon as they are resumed. Q10's 20 s limit must not count the quiet (held over 22 s).
BB_HEAVY_QUICK_LIMIT=20 "$heavy" --pool quick Q10 -- sh -c "$(hold "[ -e '$dir/q-go' ]")" > /dev/null 2> "$dir/q10.err" &
q10=$!
"$heavy" --pool quick Q11 -- sh -c "$(hold "[ -e '$dir/q-go' ]"); exit 3" > /dev/null 2> "$dir/q11.err" &
q11=$!
wait_for "B10 never ran" test -s "$dir/b10"
both_quick_held() { local held; held=$("$heavy" status); [[ $held == *'held by Q10 '* && $held == *'held by Q11 '* ]]; }
wait_for "Q10/Q11 never started" both_quick_held
# P10 measures B10 once every job is paused, then keeps the quiet until released.
"$heavy" --pool playtest --gpu exclusive --cpu-quiet P10 -- sh -c "
    $(hold "grep -q 'paused Q11' '$dir/p10.err' && grep -q 'paused B10' '$dir/p10.err'")
    a=\$(wc -l < '$dir/b10'); sleep 1.5; b=\$(wc -l < '$dir/b10'); echo \$a \$b > '$dir/p10'
    $(hold "[ -e '$dir/p10-release' ]")" 2> "$dir/p10.err" &
p10=$!
wait_for "P10 never measured: $(cat "$dir/p10.err" 2>/dev/null)" test -s "$dir/p10"
( "$heavy" --pool sweep B11 -- true 2>/dev/null; echo done > "$dir/b11" ) &  # its slot is free, but the CPU is quiet
b11=$!
# The quiet outlasts Q10's 20 s limit (a lower bound: 22 s more); Q10/Q11 get their go-file while stopped.
sleep 22
touch "$dir/q-go" "$dir/p10-release"
wait "$p10" || fail "P10 failed: $(cat "$dir/p10.err")"
read -r a b < "$dir/p10"
(( a == b )) || fail "a build job kept running during a CPU-quiet perf run ($a -> $b ticks)"
grep -q 'P10: CPU quiet: paused B10' "$dir/p10.err" || fail "pause not reported: $(cat "$dir/p10.err")"
grep -q 'P10: CPU quiet: paused Q10' "$dir/p10.err" || fail "quick job pause not reported"
grew "$dir/b10" || fail "a paused build job was not resumed"
wait_for "resume not reported: $(cat "$dir/p10.err")" grep -q 'P10: CPU quiet over: resumed B10' "$dir/p10.err"
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
    assert e['status'] == status and e['paused_by'] == ['P10'] and e['paused_s'] > 0, e
    assert not os.path.exists(os.path.join(sys.argv[2], f'paused-{pids[label]}.jsonl')), label
PY
wait "$b11" || true
[[ -e $dir/b11 ]] || fail "B11 never ran"
order quiet-end:P10 start:B11  # a job asking during the quiet started only after it
kill "$b10" 2>/dev/null; wait "$b10" 2>/dev/null || true; pkill -f "$dir/b10" 2>/dev/null || true
rm -f "$dir/b10"
"$heavy" B12 -- sh -c "while :; do echo t >> '$dir/b12'; sleep 0.2; done" &
b12=$!
wait_for "B12 never ran" test -s "$dir/b12"
BB_HEAVY_QUIET_MAX=2 "$heavy" --pool playtest --gpu exclusive --cpu-quiet P11 -- sh -c "$(hold "[ -e '$dir/p11-release' ]")" 2> "$dir/p11.err" &
p11=$!
wait_for "bound not reported loudly: $(cat "$dir/p11.err" 2>/dev/null)" grep -q 'CPU QUIET BOUND REACHED' "$dir/p11.err"
grew "$dir/b12" || fail "the CPU-quiet bound did not resume the build job"
touch "$dir/p11-release"
wait "$p11" || fail "P11 failed: $(cat "$dir/p11.err")"
kill "$b12" 2>/dev/null; wait "$b12" 2>/dev/null || true; pkill -f "$dir/b12" 2>/dev/null || true
# A process another pause already stopped is left to it: a CPU-quiet run neither counts nor resumes it
# (a real perf run pausing a full --test, then resuming everything, must not resume what this
# test's own perf run paused).
"$heavy" B13 -- sh -c "while :; do echo t >> '$dir/b13'; sleep 0.2; done" &
b13=$!
wait_for "B13 never ran" test -s "$dir/b13"
b13_loop=$(pgrep -f "^sh -c while :; do echo t >> '$dir/b13'" || true)
[[ -n $b13_loop ]] || fail "B13's loop not found"
kill -STOP "$b13_loop"
"$heavy" --pool playtest --gpu exclusive --cpu-quiet P12 -- sh -c "$(hold "grep -q 'paused B13' '$dir/p12.err'")" \
    2> "$dir/p12.err" || fail "P12 failed: $(cat "$dir/p12.err")"
wait_for "P12's quiet never ended: $(cat "$dir/p12.err")" grep -q 'P12: CPU quiet over: resumed B13' "$dir/p12.err"
[[ $(ps -o stat= -p "$b13_loop") == T* ]] || fail "a CPU-quiet run resumed a process another pause had stopped"
grep -q 'P12: CPU quiet: paused B13 (build0, [0-9]* processes; 1 already stopped by another pause, left to it)' "$dir/p12.err" ||
    fail "already-stopped process not reported: $(cat "$dir/p12.err")"
kill -CONT "$b13_loop"
grew "$dir/b13" || fail "B13 did not run after its own pauser resumed it"
kill "$b13" 2>/dev/null; wait "$b13" 2>/dev/null || true; pkill -f "$dir/b13" 2>/dev/null || true
# A job that ends the moment it is resumed still reads the whole pause: the end is written before
# SIGCONT, and no pause file is left behind. Q14 waits for a file P13 creates while Q14 is stopped.
"$heavy" --pool quick Q14 -- sh -c "$(hold "[ -e '$dir/q14-go' ]")" 2> "$dir/q14.err" &
q14=$!
until_held 'quick[01]' Q14
"$heavy" --pool playtest --gpu exclusive --cpu-quiet P13 -- sh -c "$(hold "grep -q 'paused Q14' '$dir/p13.err'"); touch '$dir/q14-go'" \
    2> "$dir/p13.err" || fail "P13 failed: $(cat "$dir/p13.err")"
wait "$q14" || fail "Q14 failed: $(cat "$dir/q14.err")"
python3 - "$dir/history.jsonl" "$dir" <<'PY' || fail "a job ending at once after its resume lost its pause: $(cat "$dir/q14.err")"
import json, os, sys
events = [json.loads(line) for line in open(sys.argv[1])]
end = [e for e in events if e.get('event') == 'end' and e['label'] == 'Q14'][-1]
pid = [e for e in events if e.get('event') == 'start' and e['label'] == 'Q14'][-1]['pid']
assert end['paused_by'] == ['P13'] and end['paused_s'] > 0, end  # the whole pause, closed before the resume
assert not os.path.exists(os.path.join(sys.argv[2], f'paused-{pid}.jsonl')), 'pause file left behind'
PY
# --cpu-quiet=ecore: a running build job is not paused but moved to background QoS (E-cores; ps
# priority 4, read back), a process it forks meanwhile inherits that, a new job starts at once in
# the background, and at the end the demoted processes come back and the job's end says so.
pri() { ps -o pri= -p "$1" | tr -d ' '; }
BB_HEAVY_POLITE=0 "$heavy" B15 -- sh -c "while :; do echo t >> '$dir/b15'; sleep 0.2; done" 2> "$dir/b15.err" &
b15=$!
for _ in $(seq 50); do [[ -s $dir/b15 ]] && break; sleep 0.1; done
b15_loop=$(pgrep -f "^sh -c while :; do echo t >> '$dir/b15'" || true)
[[ -n $b15_loop ]] || fail "B15's loop not found"
normal=$(pri "$b15_loop")
(( normal > 4 )) || fail "B15 started in the background already (priority $normal)"
"$heavy" --pool playtest --gpu exclusive --cpu-quiet=ecore P15 -- sleep 4 2> "$dir/p15.err" &
p15=$!
sleep 2
demoted=$(pri "$b15_loop")
child_pri=none
for _ in $(seq 20); do  # the loop's sleep 0.2, forked after the demotion (between its sleeps for an instant)
    child=$(pgrep -P "$b15_loop" | head -1 || true)
    if [[ -n $child ]]; then child_pri=$(pri "$child"); [[ -n $child_pri ]] && break; fi
    child_pri=none; sleep 0.05
done
before=$(ticks "$dir/b15"); sleep 1; after=$(ticks "$dir/b15")
start=$(date +%s)
b16=$("$heavy" --pool sweep B16 -- sh -c 'ps -o pri= -p $$' 2> /dev/null | tr -d ' ')
b16_wait=$(( $(date +%s) - start ))
wait "$p15"
sleep 1.5
restored=$(pri "$b15_loop")
(( demoted == 4 )) || fail "a running build job not at background QoS under --cpu-quiet=ecore (priority $demoted)"
[[ $child_pri == 4 ]] || fail "a process forked during the E-core quiet did not inherit it (priority $child_pri)"
(( after > before )) || fail "a build job stopped under --cpu-quiet=ecore ($before -> $after ticks)"
[[ $b16 == 4 ]] || fail "a job started during the E-core quiet is not in the background (priority '$b16')"
(( b16_wait < 3 )) || fail "a job waited ${b16_wait} s to start during the E-core quiet"
(( restored == normal )) || fail "B15 not restored after the E-core quiet (priority $restored, was $normal)"
grep -q 'P15: CPU quiet (E-cores): B15 (build0) at background QoS' "$dir/p15.err" || fail "demotion not reported: $(cat "$dir/p15.err")"
grep -q 'P15: CPU quiet over: B15 back from E-cores' "$dir/p15.err" || fail "restore not reported: $(cat "$dir/p15.err")"
kill "$b15" 2>/dev/null; wait "$b15" 2>/dev/null || true; pkill -f "$dir/b15" 2>/dev/null || true
grep -Eq '^heavy: B15: ran [0-9]+ s at E-core QoS for perf runs \(P15\); ended with' "$dir/b15.err" ||
    fail "B15's end does not name the E-core quiet: $(cat "$dir/b15.err")"
# The same through ninja (posix_spawn, as the GPU build compiles): the job ninja starts after the
# demotion inherits background QoS; one it started before is demoted by the watcher's rescan.
command -v ninja > /dev/null || fail "ninja not on PATH"
mkdir -p "$dir/nj" && printf 'rule s\n  command = sleep $t\nbuild a: s\n  t = 1\nbuild b: s\n  t = 4\n' > "$dir/nj/build.ninja"
BB_HEAVY_POLITE=0 "$heavy" B17 -- ninja -C "$dir/nj" -j1 a b > /dev/null 2>&1 &
b17=$!
until_held build0 B17
b17_ninja=''
for _ in $(seq 100); do
    b17_ninja=$(pgrep -f "^ninja -C $dir/nj" | head -1 || true)
    [[ -n $b17_ninja ]] && pgrep -P "$b17_ninja" > /dev/null && break; sleep 0.05
done
[[ -n $b17_ninja ]] || fail "B17's ninja never started its first job"
"$heavy" --pool playtest --gpu exclusive --cpu-quiet=ecore P16 -- sleep 3 2> /dev/null &
p16=$!
sleep 2.2  # job a (sleep 1) ended during the quiet; ninja spawned job b after the demotion
job_b=$(pgrep -P "$b17_ninja" | head -1 || true)
job_b_pri=$( [[ -n $job_b ]] && pri "$job_b" || echo none)
wait "$p16"; wait "$b17" || fail "B17 (ninja) failed"
[[ $job_b_pri == 4 ]] || fail "a job ninja spawned during the E-core quiet is not in the background (priority $job_b_pri)"
# A job's process whose parent exited (re-parented to launchd, out of the job's tree) is still in
# the job's own process group: CPU quiet stops it, and under ecore demotes it and the children it
# keeps forking.
BB_HEAVY_POLITE=0 "$heavy" B18 -- sh -c "(sh -c 'while :; do echo t >> $dir/b18; sleep 0.2; done' &); while :; do sleep 0.5; done" &
b18=$!
for _ in $(seq 50); do [[ -s $dir/b18 ]] && break; sleep 0.1; done
orphan=$(pgrep -f "^sh -c while :; do echo t >> $dir/b18" || true)
[[ -n $orphan && $(ps -o ppid= -p "$orphan" | tr -d ' ') == 1 ]] || fail "B18's loop is not an orphan (pid '$orphan')"
"$heavy" --pool playtest --gpu exclusive --cpu-quiet P21 -- sh -c "sleep 0.5; a=\$(wc -l < '$dir/b18'); sleep 1.5; b=\$(wc -l < '$dir/b18'); echo \$a \$b > '$dir/p21'" 2> /dev/null
read -r a b < "$dir/p21"
(( a == b )) || fail "an orphaned process of a job kept running during CPU quiet ($a -> $b ticks)"
"$heavy" --pool playtest --gpu exclusive --cpu-quiet=ecore P22 -- sleep 2.5 2> /dev/null &
p22=$!
sleep 1.6
orphan_pri=$(pri "$orphan")
orphan_child=none
for _ in $(seq 20); do
    c=$(pgrep -P "$orphan" | head -1 || true)
    if [[ -n $c ]]; then orphan_child=$(pri "$c"); [[ -n $orphan_child ]] && break; fi
    orphan_child=none; sleep 0.05
done
wait "$p22"
(( orphan_pri == 4 )) && [[ $orphan_child == 4 ]] ||
    fail "an orphaned process (priority $orphan_pri) or its new child ($orphan_child) not demoted under ecore"
kill "$b18" 2>/dev/null; wait "$b18" 2>/dev/null || true
sleep 0.3; ! kill -0 "$orphan" 2>/dev/null || fail "the job's orphan outlived the job's SIGTERM (group forwarding)"
# A SIGKILL to the caller's process group (a stopped task, a killed shell) takes the job's command
# down too, though it runs in its own session: its reaper SIGKILLs the job's group once heavy.py is
# gone, so the slot is never free while the job still runs. Build and quick pools.
for pool in build quick; do
    tag=k$pool
    perl -e 'setpgrp(0, 0); exec @ARGV' -- "$heavy" --pool $pool K20$pool -- \
        sh -c "while :; do echo t >> '$dir/$tag'; sleep 0.2; done" 2> /dev/null &
    launcher=$!
    for _ in $(seq 100); do [[ -s $dir/$tag ]] && break; sleep 0.05; done
    loop=$(pgrep -f "^sh -c while :; do echo t >> '$dir/$tag'" || true)
    [[ -n $loop ]] || fail "K20$pool's command never started"
    kill -KILL -- -"$launcher"
    for _ in $(seq 40); do kill -0 "$loop" 2>/dev/null || break; sleep 0.1; done
    ! kill -0 "$loop" 2>/dev/null || { kill "$loop"; fail "$pool job's command survived a SIGKILL to the caller's group"; }
    "$heavy" status | grep -q "held by K20$pool" && fail "$pool slot still held after the job's group died"
    wait "$launcher" 2>/dev/null || true
done
# Restore after ecore returns a polite job to its own scheduling (a priority-lane job: nice 5,
# utility QoS), not merely out of the background.
"$heavy" --priority B19 -- sh -c "while :; do echo t >> '$dir/b19'; sleep 0.2; done" 2> /dev/null &
b19=$!
for _ in $(seq 50); do [[ -s $dir/b19 ]] && break; sleep 0.1; done
b19_loop=$(pgrep -f "^sh -c while :; do echo t >> '$dir/b19'" || true)
[[ -n $b19_loop ]] || fail "B19's loop not found"
b19_before="$(ps -o pri=,nice= -p "$b19_loop" | tr -s ' ')"
"$heavy" --pool playtest --gpu exclusive --cpu-quiet=ecore P23 -- sleep 2 2> /dev/null &
p23=$!; sleep 1.5; b19_during=$(pri "$b19_loop"); wait "$p23"; sleep 1.5
b19_after="$(ps -o pri=,nice= -p "$b19_loop" | tr -s ' ')"
(( b19_during == 4 )) || fail "the priority-lane job was not demoted (priority $b19_during)"
[[ $b19_after == "$b19_before" ]] || fail "restore changed the priority-lane job's scheduling: '$b19_before' -> '$b19_after'"
kill "$b19" 2>/dev/null; wait "$b19" 2>/dev/null || true
# --settle LOAD,SECONDS: a quiet run waits for the 1-minute load to fall below LOAD, or refuses
# (exit 75) past SECONDS, naming the top processes; the quiet start records the top 5 CPU
# processes; a GPU request is recorded before its grant; --settle needs --cpu-quiet.
"$heavy" --pool playtest --gpu exclusive --cpu-quiet --settle 1000,5 P17 -- true 2> "$dir/p17.err" ||
    fail "a run with a reachable --settle failed: $(cat "$dir/p17.err")"
grep -q 'P17: settled: 1-minute load' "$dir/p17.err" || fail "settle not reported: $(cat "$dir/p17.err")"
set +e; "$heavy" --pool playtest --gpu exclusive --cpu-quiet --settle 0.001,2 P18 -- touch "$dir/p18-ran" 2> "$dir/p18.err"; p18=$?; set -e
(( p18 == 75 )) || fail "an unreachable --settle exited $p18, expected 75"
[[ ! -e $dir/p18-ran ]] || fail "the run started although the load never settled"
grep -q 'P18: SETTLE FAILED: 1-minute load .* still at or above 0.001 after 2 s; top: ' "$dir/p18.err" ||
    fail "settle failure not explained: $(cat "$dir/p18.err")"
if "$heavy" --settle 1,5 P19 -- true 2>/dev/null; then fail "--settle without --cpu-quiet accepted"; fi
python3 - "$dir/history.jsonl" <<'PY' || fail "settle, quiet-start top or gpu-request events in history.jsonl"
import json, sys
events = [json.loads(line) for line in open(sys.argv[1])]
settles = {e['label']: e for e in events if e.get('event') == 'settle'}
assert settles['P17']['settled'] is True and settles['P18']['settled'] is False and settles['P18']['top'], settles
starts = [e for e in events if e.get('event') == 'quiet-start' and e['by'] == 'P17']
assert starts and len(starts[0]['top']) == 5 and starts[0]['mode'] == 'stop', starts
assert all(' pgid ' in t and ' cwd ' in t for t in starts[0]['top']), starts[0]['top']
order = [e['event'] for e in events if e.get('label') == 'P17' and e['event'] in ('gpu-request', 'gpu')]
assert order[:2] == ['gpu-request', 'gpu'], order
PY
if "$heavy" --cpu-quiet K1 -- true 2>/dev/null; then fail "--cpu-quiet without --gpu exclusive accepted"; fi
if "$heavy" K2 -- "$heavy" --pool playtest --gpu exclusive --cpu-quiet K3 -- true 2>/dev/null; then
    fail "--cpu-quiet inside a slot accepted"
fi

# Job records: a start event with the command and the directory; an end event for every pool with
# the exit status, or the signal that killed it (SIGTERM to heavy.sh is forwarded to the command);
# a SIGKILLed heavy.sh's command is reaped; only if its reaper dies too does it run on, ORPHANED.
"$heavy" R1 -- sh -c 'exit 7' 2>/dev/null || true
"$heavy" R2 -- sleep 30 &
r2=$!
until_held build0 R2
kill -TERM "$r2"; wait "$r2" 2>/dev/null || true
wait_for "SIGTERM to heavy.sh did not end its command" slot_free build0
reaper_of() { python3 -c 'import json, sys; print(json.load(open(sys.argv[1]))["command_pid"])' "$dir/build0"; }
# A SIGKILLed heavy.sh: the command's reaper ends the command and the slot is free.
"$heavy" R3 -- sleep 30 &
r3=$!
until_held build0 R3
r3_cmd=$(pgrep -P "$(reaper_of)" | head -1 || true)
[[ -n $r3_cmd ]] || fail "R3's command not found under its reaper"
kill -9 "$r3"; wait "$r3" 2>/dev/null || true
gone() { ! kill -0 "$1" 2>/dev/null; }
wait_for "a SIGKILLed heavy.sh's command was not reaped" gone "$r3_cmd"
wait_for "slot not free after the reaped job: $("$heavy" status | grep build0)" slot_free build0
# Only when heavy.sh and the reaper both die does a command outlive its job: shown as ORPHANED.
"$heavy" R4 -- sleep 30 &
r4=$!
until_held build0 R4
r4_reaper=$(reaper_of)
r4_cmd=$(pgrep -P "$r4_reaper" | head -1 || true)
[[ -n $r4_cmd ]] || fail "R4's command not found under its reaper"
kill -9 "$r4" "$r4_reaper"; wait "$r4" 2>/dev/null || true
orphan_shown() { "$heavy" status | grep -q '^build0: held by R4 .*ORPHANED'; }
wait_for "orphaned command not shown: $("$heavy" status | grep build0)" orphan_shown
kill "$r4_cmd"
wait_for "build0 not freed after the orphan ended" slot_free build0
python3 - "$dir/history.jsonl" "$PWD" <<'PY' || fail "job records in history.jsonl"
import json, os, sys
lines = [json.loads(l) for l in open(sys.argv[1])]
start = {l['label']: l for l in lines if l['event'] == 'start'}
end = {l['label']: l for l in lines if l['event'] == 'end'}
# The directory as the kernel names it (a checkout under /tmp is /private/tmp to getcwd()).
assert start['R1']['command'].endswith("sh -c exit 7") and \
    os.path.realpath(start['R1']['cwd']) == os.path.realpath(sys.argv[2]), start['R1']
assert end['R1']['status'] == 7 and end['R1']['pool'] == 'build' and 'seconds' in end['R1'], end['R1']
assert end['R2']['signal'] == 'SIGTERM' and end['R2']['status'] == 128 + 15, end['R2']
assert 'R3' not in end and 'R4' not in end  # heavy.sh SIGKILLed: no end event (R4: ORPHANED)
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
assert any(l['label'] == 'B' for l in starts), 'B started'  # after A ended: checked by order above
assert all({'pool', 'slot', 'waited_s', 'load', 'held'} <= set(l) for l in starts)
assert any(l['label'] == 'Q4' and l['status'] == 124 for l in ends), 'Q4 killed'
PY
echo "heavy.sh: history log, build/playtest/quick pools, quick hints and time limit, waiting, priority slot, GPU shared/exclusive lock with gate, start announcement, polite scheduling, idle-aware admission, CPU-quiet pause (leaving another pause's processes stopped; named at the job's end), --cpu-quiet=ecore (background QoS read back, inherited, restored), --settle, quiet-start top processes, GPU request events, job records (command, directory, end status/signal, orphans), one job per worktree and INVALID on a changed tree, sweep lane, release on SIGKILL, nesting, exit status, usage errors PASS"
