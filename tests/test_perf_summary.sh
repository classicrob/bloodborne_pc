#!/bin/sh
# tools/macos/perf_summary.py on a synthetic BB_PERF file: --after-line/--before-line select the
# windows of one perf_run.sh segment, and a range without windows is an error.
set -eu
cd "$(dirname "$0")/.."
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
python3 - "$dir/perf.jsonl" <<'PY'
import json, sys
def window(t, flips):
    return {'type': 'window', 'elapsed_seconds': t, 'seconds': 5.0, 'flips': flips, 'metrics': {},
            'gpu': {'busy_union_ms': flips * 10.0, 'busy_union_ms_per_flip': 10.0, 'main_submissions': flips,
                    'deferred_reads': 0}}
with open(sys.argv[1], 'w') as f:
    f.write(json.dumps({'type': 'schema', 'version': 3, 'perturbed': False}) + '\n')
    for i, flips in enumerate([50, 100, 150, 200]):  # 10, 20, 30, 40 FPS
        f.write(json.dumps(window(5.0 * (i + 1), flips)) + '\n')
PY
summary=tools/macos/perf_summary.py
python3 "$summary" "$dir/perf.jsonl" | grep -q 'total: 20.0 s, 500 flips, 25.00 FPS' || { echo "perf_summary: whole file wrong" >&2; exit 1; }
# Lines 3-4 are the 20 and 30 FPS windows: a segment from line 2 to line 4.
python3 "$summary" --after-line 2 --before-line 4 "$dir/perf.jsonl" | grep -q 'total: 10.0 s, 250 flips, 25.00 FPS' ||
    { echo "perf_summary: segment range wrong" >&2; exit 1; }
python3 "$summary" --after-line 4 "$dir/perf.jsonl" | grep -q 'total: 5.0 s, 200 flips, 40.00 FPS' ||
    { echo "perf_summary: --after-line wrong" >&2; exit 1; }
if python3 "$summary" --after-line 3 --before-line 3 "$dir/perf.jsonl" 2>/dev/null; then
    echo "perf_summary: an empty segment was accepted" >&2; exit 1
fi
echo "perf_summary.py: whole file, segment line ranges, empty range is an error PASS"
