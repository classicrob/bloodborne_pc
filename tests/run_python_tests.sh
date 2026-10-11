#!/usr/bin/env bash
# Every tests/test_*.py unittest (Darwin build.sh --test). A skipped test fails the run: on this
# platform a skip would hide an untested path.
set -euo pipefail
cd "$(dirname "$0")/.."
log=out/python-tests.log
mkdir -p out
if ! python3 -m unittest discover -s tests > "$log" 2>&1; then
    tail -40 "$log"
    echo "Python tests FAILED (log: $log)" >&2
    exit 1
fi
summary=$(grep -E '^(Ran |OK|NO TESTS RAN)' "$log" | tr '\n' ' ')
if grep -qE 'skipped=|NO TESTS RAN' "$log"; then
    echo "Python tests: a test was skipped or none ran: $summary" >&2
    exit 1
fi
echo "Python tests: $summary"
