#!/usr/bin/env bash
# BB_PERF run of the tracked production route (docs/macos/perf.md): the route itself is
# tools/macos/play_route.py; this records the host state around it and summarizes perf.jsonl.
#   tools/macos/perf_run.sh --probe PATH [--output 1080p|4k] [--phases] [--label NAME] [--until STAGE]
#                           [--fixture NAME] [--warm-cache NAME] [--diag-env BB_X=V]...
#                           [--hold SECONDS | --segments LIST]
# --hold keeps the game at the last stage for SECONDS, then stops it; the summary then covers
# only the windows completed during the hold (the stage itself, not loading on the way).
# --segments still:60,walk:60 runs the segments in order at the last stage, each with its own
# summary: still leaves the pad neutral; walk holds the left stick forward while the right stick
# turns the camera one way, then the other, every 3 s; turn only turns the camera that way
# (BB_PAD_FILE). A segment's summary covers
# the windows written from 6 s after its start (a 5 s window never straddles two segments) to
# its end. --fixture, --warm-cache and --diag-env (repeatable) pass to play_route.py.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
# The run takes the GPU exclusively (tools/macos/heavy.sh): it waits until running games end, and
# games started meanwhile wait for it. The game itself runs under this hold.
if [[ ${BB_HEAVY_GPU:-} != exclusive ]]; then
    exec tools/macos/heavy.sh --pool playtest --gpu exclusive "perf_run $*" -- bash "$PWD/tools/macos/perf_run.sh" "$@"
fi
probe='' output=4k phases=0 label=perf until=() hold=0 segments='' seeds=()
while (($#)); do
    case $1 in
    --probe) probe=$2; shift 2 ;;
    --output) output=$2; shift 2 ;;
    --phases) phases=1; shift ;;
    --label) label=$2; shift 2 ;;
    --until) until=(--until "$2"); shift 2 ;;
    --hold) hold=$2; shift 2 ;;
    --segments) segments=$2; shift 2 ;;
    --fixture|--warm-cache|--diag-env) seeds+=("$1" "$2"); shift 2 ;;
    *) echo "perf_run: unknown argument $1" >&2; exit 2 ;;
    esac
done
fail() { echo "perf_run: $*" >&2; exit 1; }
[[ -n $probe && -x $probe ]] || fail "--probe must name the signed production bb-probe"
library=$(dirname -- "$probe")/gpu/libbbgpu.dylib
[[ -f $library ]] || fail "no $library beside the probe"
python3 tools/macos/play_route.py route --help | grep -q -- '--perf' ||
    fail "tools/macos/play_route.py has no --perf option (needed to pass BB_PERF to the game)"
# The GPU and the Rosetta budget belong to this run: no other game may be running.
if others=$(pgrep -x bb-probe); then
    fail "another bb-probe is running (pid $(echo "$others" | tr '\n' ' ')); a perf run needs the GPU to itself"
fi
host_state() {
    python3 - "$1" "$probe" "$library" <<'PY'
import hashlib, json, os, subprocess, sys
out, probe, library = sys.argv[1:4]
def sha(path):
    with open(path, 'rb') as f:
        return hashlib.sha256(f.read()).hexdigest()
top = subprocess.run(['ps', '-Ao', 'pid=,pcpu=,comm=', '-r'], capture_output=True, text=True, check=True)
json.dump({
    'loadavg': os.getloadavg(),
    'uptime': subprocess.run(['uptime'], capture_output=True, text=True, check=True).stdout.strip(),
    'top_cpu': [line.split(None, 2) for line in top.stdout.splitlines()[:12]],
    'commit': subprocess.run(['git', 'rev-parse', 'HEAD'], capture_output=True, text=True, check=True).stdout.strip(),
    'probe': {'path': os.path.realpath(probe), 'sha256': sha(probe)},
    'library': {'path': os.path.realpath(library), 'sha256': sha(library)},
}, open(out, 'w'), indent=1)
PY
}
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
host_state "$scratch/host-before.json"
flags=(--perf)
if ((phases)); then flags+=(--perf-phases); fi
[[ $hold =~ ^[0-9]+$ ]] || fail "--hold must be a whole number of seconds"
plan=()
if [[ -n $segments ]]; then
    ((hold == 0)) || fail "--hold and --segments exclude each other"
    IFS=, read -r -a plan <<< "$segments"
    for segment in "${plan[@]}"; do
        [[ $segment =~ ^(still|walk|turn):([0-9]+)$ ]] && ((BASH_REMATCH[2] >= 16)) ||
            fail "segment '$segment': expected still:, walk: or turn:SECONDS, at least 16 s"
    done
    hold=1 # keep the game for the segments
fi
if ((hold)); then flags+=(--keep); fi
status=0
python3 tools/macos/play_route.py route --probe "$probe" --output "$output" "${flags[@]}" \
    --label "$label" ${until[@]+"${until[@]}"} ${seeds[@]+"${seeds[@]}"} || status=$?
run=$(cd out/macos_route && cd "$(readlink current)" && pwd)
summary=()
if ((hold && status != 0)); then
    python3 tools/macos/play_route.py stop || true # the route failed; don't leave the game running
fi
lines() { wc -l < "$run/perf.jsonl" | tr -d ' '; }
pad() { printf '%s\n' "$1" > "$run/pad.txt"; }
# The game must stay alive through a hold or segment: a game that stops (an assertion, a crash)
# fails the run instead of leaving a short summary that looks like a measurement.
game=''
if ((hold && status == 0)); then
    game=$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1])).get("pid") or "")' "$run/manifest.json")
    [[ -n $game ]] || fail "no game pid in $run/manifest.json"
fi
check_game() {
    kill -0 "$game" 2>/dev/null && return
    cp "$scratch/host-before.json" "$run/host-before.json"
    fail "the game stopped during $1 ($(cat "$run/exit.json" 2>/dev/null || echo 'no exit record')); see $run/game.log"
}
if ((${#plan[@]} && status == 0)); then
    [[ -f $run/perf.jsonl ]] || fail "no $run/perf.jsonl after the route: BB_PERF is not active in this build"
    : > "$run/segments.txt"
    for segment in "${plan[@]}"; do
        kind=${segment%%:*} length=${segment#*:} start=$SECONDS first=''
        echo "perf_run: segment $segment"
        while ((SECONDS - start < length)); do
            check_game "segment $segment"
            if [[ -z $first ]] && ((SECONDS - start >= 6)); then first=$(lines); fi
            if [[ $kind == walk ]]; then
                if (((SECONDS - start) / 3 % 2)); then pad 'ly=0 rx=170'; else pad 'ly=0 rx=86'; fi
            elif [[ $kind == turn ]]; then
                if (((SECONDS - start) / 3 % 2)); then pad 'rx=200'; else pad 'rx=56'; fi
            fi
            sleep 1
        done
        pad ''
        echo "$segment ${first:-$(lines)} $(lines)" >> "$run/segments.txt"
    done
    python3 tools/macos/play_route.py stop || status=$?
elif ((hold && status == 0)); then
    [[ -f $run/perf.jsonl ]] || fail "no $run/perf.jsonl after the route: BB_PERF is not active in this build"
    first=$(wc -l < "$run/perf.jsonl")
    echo "perf_run: holding the last stage for $hold s (records after line $first)"
    end=$((SECONDS + hold))
    while ((SECONDS < end)); do check_game "the hold"; sleep 1; done
    python3 tools/macos/play_route.py stop || status=$?
    summary=(--after-line "$first")
fi
cp "$scratch/host-before.json" "$run/host-before.json"
host_state "$run/host-after.json"
((status == 0)) || fail "route failed (exit $status); see $run"
[[ -s $run/perf.jsonl ]] || fail "no $run/perf.jsonl: the game did not record BB_PERF windows"
if ((${#plan[@]})); then
    : > "$run/perf-summary.txt"
    while read -r segment first last; do
        echo "== segment $segment (perf.jsonl lines $((first + 1))-$last)" | tee -a "$run/perf-summary.txt"
        python3 tools/macos/perf_summary.py --after-line "$first" --before-line "$last" "$run/perf.jsonl" |
            tee -a "$run/perf-summary.txt"
    done < "$run/segments.txt"
else
    python3 tools/macos/perf_summary.py ${summary[@]+"${summary[@]}"} "$run/perf.jsonl" | tee "$run/perf-summary.txt"
fi
echo "perf_run: $run"
