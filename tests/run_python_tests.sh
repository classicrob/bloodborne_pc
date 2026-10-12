#!/usr/bin/env bash
# Every tests/test_*.py unittest (Darwin build.sh --test). A skipped test fails the run: on this
# platform a skip would hide an untested path.
set -euo pipefail
cd "$(dirname "$0")/.."
log=out/python-tests.log
mkdir -p out
# The binaries the tests launch must be this tree's: newer than the GPU library and every runtime
# source (a stale one from an older --test ran against a newer libbbgpu and failed in dyld).
stale=()
for exe in out/bb-probe out/runtime-test out/sema-test out/content-test; do
    if [[ ! -e $exe ]]; then stale+=("$exe: missing"); continue; fi
    name=${exe#out/}; name=${name%-test}
    for input in out/gpu/libbbgpu.dylib src/*.c src/*.h tests/test_"$name".c; do
        if [[ -e $input && $input -nt $exe ]]; then stale+=("$exe: older than $input"); break; fi
    done
done
if ((${#stale[@]})); then
    echo 'Python tests: stale or missing test binaries (bash build.sh --test builds them):' >&2
    printf '  %s\n' "${stale[@]}" >&2
    exit 1
fi
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
