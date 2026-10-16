#!/usr/bin/env bash
# tools/macos/preview.sh on a scratch tree: a fake build.sh (two tiny signed binaries; the test gate
# set by FAKE_TEST) and the real heavy.sh in a private lock directory. Checks: a dirty tree and a
# second preview of one commit are refused; --build-only stages (hashes, signatures, smoke) and
# writes PREVIEW.txt; the gate's PASS, FAILED (with the --test-one rerun after a perf-run pause)
# and skips (exit 3, not a gate); bad usage exits 2.
set -euo pipefail
cd "$(dirname "$0")/.."
root=$PWD
dir=$(mktemp -d); trap 'rm -rf "$dir"' EXIT
fail() { echo "preview test FAILED: $*" >&2; exit 1; }
unset ${!BB_HEAVY_@}
export BB_HEAVY_DIR=$dir/locks BB_HEAVY_SHARED_TREE=1 BB_HEAVY_IDLE_SECONDS=0
export BB_HEAVY_HELD='build0:preview fixture'
mkdir -p "$BB_HEAVY_DIR" "$dir/stage"
tree=$dir/trees/t && mkdir -p "$tree/tools/macos" "$tree/out/gpu"
cp tools/macos/heavy.py tools/macos/preview.sh "$tree/tools/macos/"
# heavy.sh: the real heavy.py; FAKE_SMOKE_PAUSED appends the line heavy prints when a perf run paused the job.
cat > "$tree/tools/macos/heavy.sh" <<'WRAP'
#!/bin/sh
python3 "$(dirname -- "$0")/heavy.py" "$@"; status=$?
case "$*" in *smoke*) [ -n "${FAKE_SMOKE_PAUSED:-}" ] &&
    echo "heavy: x smoke: PAUSED 3 s for perf runs (R0); ended with exit $status" >&2 ;; esac
exit $status
WRAP
chmod +x "$tree/tools/macos/heavy.sh"
printf '#include <stdio.h>\nint main(void){puts("Vulkan: fake; command submission PASS");return 0;}\n' > "$dir/probe.c"
cc "$dir/probe.c" -o "$dir/bb-probe" && echo 'int bbgpu_fake;' > "$dir/gpu.c" && cc -dynamiclib "$dir/gpu.c" -o "$dir/libbbgpu.dylib"
cat > "$tree/build.sh" <<FAKE
#!/usr/bin/env bash
case \${1:-} in
'') cp "$dir/bb-probe" out/bb-probe; cp "$dir/libbbgpu.dylib" out/gpu/libbbgpu.dylib
    if [[ -n \${FAKE_MUTATE_SOURCE:-} ]]; then echo changed >> tools/macos/preview.sh; fi
    echo 'GPU library: ccache=off'; echo 'Darwin GPU host imports verified: bb-probe (27 symbols)' ;;
--test-keep-going)
    case \$FAKE_TEST in
    pass) echo 'test-keep-going: 5 run, 5 passed, 0 failed, 0 skipped (out/test-results.txt)'; exit 0 ;;
    fail) echo 'test-keep-going: 5 run, 4 passed, 1 failed, 0 skipped (out/test-results.txt)'
          echo '  FAILED: exit 1: sh tests/test_x.sh'; exit 1 ;;
    skip) echo 'test-keep-going: 4 run, 4 passed, 0 failed, 1 skipped (out/test-results.txt)'
          echo "  skipped: sh tests/\$BB_TEST_SKIP.sh"; exit 3 ;;
    esac ;;
--test-one) echo "test-one: \$2: 1 run(s), exit 0"; exit 0 ;;
esac
FAKE
(cd "$tree" && git init -q && git config user.email t@t && git config user.name t && git add -A && git commit -qm one)
run() { (cd "$tree" && bash tools/macos/preview.sh "$@") > "$dir/out" 2>&1; }
head=$(git -C "$tree" rev-parse --short HEAD)

echo edit >> "$tree/build.sh"
set +e; run --stage "$dir/stage" P1; status=$?; set -e
(( status == 2 )) && grep -q 'refused: tracked files differ from HEAD' "$dir/out" || fail "dirty tree ($status): $(cat "$dir/out")"
git -C "$tree" checkout -q build.sh
echo "PASS a tree with tracked changes is refused (exit 2)"

set +e; run --stage "$dir/stage" --build-only P1; status=$?; set -e
f=$dir/stage/$head-P1/PREVIEW.txt
(( status == 0 )) && grep -q '^smoke: Vulkan: fake; command submission PASS' "$f" && grep -q '^sha256 bb-probe [0-9a-f]\{64\}$' "$f" &&
    grep -q '^signatures: codesign -v OK' "$f" && grep -q 'host imports verified: bb-probe (27 symbols)' "$f" &&
    grep -q '^test: not run (--build-only)' "$f" && grep -q "^commit $head" "$f" ||
    fail "--build-only ($status): $(cat "$dir/out")"
echo "PASS --build-only: staged, hashed, signatures and smoke checked, PREVIEW.txt"
python3 - "$dir/stage/$head-P1/manifest.json" "$(git -C "$tree" rev-parse HEAD)" <<'PY' || fail 'preview manifest provenance'
import hashlib
import json
import pathlib
import sys

manifest = json.loads(pathlib.Path(sys.argv[1]).read_text())
assert manifest['source_tree']['head'] == sys.argv[2]
assert len(manifest['source_tree']['tree']) == 40
assert len(manifest['source_tree']['tracked_diff_sha256']) == 64
assert manifest['qualification']['test_gate'] == 'NOT_RUN'
for name in ('bb-probe', 'libbbgpu.dylib'):
    artifact = manifest['artifacts'][name]
    path = pathlib.Path(artifact['path'])
    assert path.is_absolute() and path.is_file()
    assert hashlib.sha256(path.read_bytes()).hexdigest() == artifact['sha256']
PY
echo 'PASS manifest identifies staged, hashed binaries and unrun gate'
set +e; FAKE_MUTATE_SOURCE=1 run --stage "$dir/stage" --build-only P1c; status=$?; set -e
(( status == 1 )) && grep -q 'source tree changed during build' "$dir/out" ||
    fail "a source edit during build ($status): $(cat "$dir/out")"
git -C "$tree" checkout -q tools/macos/preview.sh
echo 'PASS a source edit during build is refused before staging'
set +e; run --stage "$dir/stage" --build-only P1; status=$?; set -e
(( status == 2 )) && grep -q 'exists (a preview is never overwritten)' "$dir/out" || fail "overwrite ($status)"
echo "PASS a second preview of the same name and commit is refused (exit 2)"

set +e; FAKE_SMOKE_PAUSED=1 run --stage "$dir/stage" --build-only P1b; status=$?; set -e
(( status == 0 )) && grep -q '^smoke: Vulkan: fake; command submission PASS' "$dir/stage/$head-P1b/PREVIEW.txt" ||
    fail "a smoke job paused by a perf run ($status): $(cat "$dir/out")"
echo "PASS a smoke job paused by a perf run: heavy's PAUSED line after the probe's output, still PASS"
set +e; FAKE_TEST=pass run --stage "$dir/stage" P2; status=$?; set -e
(( status == 0 )) && grep -q '^test: PASS (build.sh --test-keep-going exit 0' "$dir/stage/$head-P2/PREVIEW.txt" || fail "pass ($status): $(cat "$dir/out")"
python3 - "$dir/stage/$head-P2/manifest.json" <<'PY' || fail 'passed gate missing from manifest'
import json, sys
assert json.load(open(sys.argv[1]))['qualification']['test_gate'] == 'PASS'
PY
echo "PASS the gate passes: test: PASS, exit 0"

# A perf run paused the gate's build.sh job: its failure gets one --test-one rerun.
python3 - "$BB_HEAVY_DIR/history.jsonl" "build.sh --test-keep-going in trees/t" <<'PY'
import datetime, json, sys
with open(sys.argv[1], 'a') as f:
    later = (datetime.datetime.now() + datetime.timedelta(seconds=5)).isoformat(timespec='seconds')
    f.write(json.dumps({'time': later, 'event': 'quiet-pause', 'by': 'R0', 'label': sys.argv[2]}) + '\n')
PY
set +e; FAKE_TEST=fail run --stage "$dir/stage" P3; status=$?; set -e
f=$dir/stage/$head-P3/PREVIEW.txt
(( status == 1 )) && grep -q '^test: FAILED' "$f" && grep -q '^test FAILED: exit 1: sh tests/test_x.sh' "$f" &&
    grep -q '^test paused or demoted by perf runs: 1 time(s)' "$f" &&
    grep -q '^rerun (paused run) --test-one tests/test_x.sh: exit 0: test-one: tests/test_x.sh: 1 run(s), exit 0' "$f" ||
    fail "a failure in a paused run ($status): $(cat "$dir/out")"
echo "PASS a failure: FAILED, exit 1, the failure listed, rerun once with --test-one after a pause"

set +e; FAKE_TEST=skip run --stage "$dir/stage" --skip test_y P4; status=$?; set -e
(( status == 3 )) && grep -q '^test: NOT A FULL GATE (skips)' "$dir/stage/$head-P4/PREVIEW.txt" &&
    grep -q '^test skipped: sh tests/test_y.sh' "$dir/stage/$head-P4/PREVIEW.txt" || fail "skip ($status): $(cat "$dir/out")"
echo "PASS skips: NOT A FULL GATE, exit 3"

for bad in "P5" "--stage $dir/stage" "--stage $dir/stage a b" "--stage $dir/nowhere P5" "--stage $dir/stage --what P5" "--stage $dir/stage bad/name"; do
    set +e; run $bad; status=$?; set -e
    (( status == 2 )) || fail "bad usage '$bad' exited $status"
done
echo "PASS bad usage exits 2"
