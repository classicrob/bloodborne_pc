#!/usr/bin/env bash
# BB_PERF run of the tracked production route (docs/macos/perf.md): the route itself is
# tools/macos/play_route.py; this records the host state around it and summarizes perf.jsonl.
#   tools/macos/perf_run.sh --probe PATH [--output 1080p|4k] [--phases] [--label NAME] [--until STAGE]
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
probe='' output=4k phases=0 label=perf until=()
while (($#)); do
    case $1 in
    --probe) probe=$2; shift 2 ;;
    --output) output=$2; shift 2 ;;
    --phases) phases=1; shift ;;
    --label) label=$2; shift 2 ;;
    --until) until=(--until "$2"); shift 2 ;;
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
status=0
python3 tools/macos/play_route.py route --probe "$probe" --output "$output" "${flags[@]}" \
    --label "$label" "${until[@]}" || status=$?
run=$(cd out/macos_route && cd "$(readlink current)" && pwd)
cp "$scratch/host-before.json" "$run/host-before.json"
host_state "$run/host-after.json"
((status == 0)) || fail "route failed (exit $status); see $run"
[[ -s $run/perf.jsonl ]] || fail "no $run/perf.jsonl: the game did not record BB_PERF windows"
python3 tools/macos/perf_summary.py "$run/perf.jsonl" | tee "$run/perf-summary.txt"
echo "perf_run: $run"
