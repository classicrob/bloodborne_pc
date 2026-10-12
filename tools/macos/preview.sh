#!/usr/bin/env bash
# A preview build of this tree's HEAD for other agents: build, stage, smoke, and the test gate.
#
#   tools/macos/preview.sh --stage DIR [--build-only] [--priority] [--skip SUBSTRING] NAME
#
# Run from the root of a clean checkout (a preview is a commit: tracked changes are refused).
#  1. bash build.sh (BB_HEAVY_PRIORITY=1 with --priority);
#  2. stages out/bb-probe and out/gpu/libbbgpu.dylib to DIR/<short HEAD>-NAME/ with SHA256SUMS,
#     checks both signatures (refuses an existing directory: a preview is never overwritten);
#  3. smoke: bb-probe --vulkan-only in a heavy.sh quick slot;
#  4. unless --build-only: bash build.sh --test-keep-going, the gate (exit 0 all passed, 1 a
#     failure, 3 skips: not a gate); --skip passes BB_TEST_SKIP. If heavy.sh's history shows the
#     run paused for a perf run and a test failed, each failure is rerun once with
#     build.sh --test-one, and both results are reported (the first stands).
# Every step prints "preview: NAME: ..."; DIR/<short HEAD>-NAME/PREVIEW.txt collects the facts to
# post (commit, tree, hashes, signatures, export gate, smoke, shader cache versions, test verdict).
# Exit: 0 when everything passed (or built, staged and smoked with --build-only), 1 on a failed
# step or test, 3 when tests were skipped, 2 on bad usage.
set -uo pipefail
usage() { echo 'usage: tools/macos/preview.sh --stage DIR [--build-only] [--priority] [--skip SUBSTRING] NAME' >&2; exit 2; }
stage='' build_only='' priority=0 skip='' name=''
while (($#)); do
    case $1 in
    --stage) (($# > 1)) || usage; stage=$2; shift 2 ;;
    --build-only) build_only=1; shift ;;
    --priority) priority=1; shift ;;
    --skip) (($# > 1)) || usage; skip=$2; shift 2 ;;
    -*) usage ;;
    *) [[ -z $name ]] || usage; name=$1; shift ;;
    esac
done
[[ -n $stage && -n $name && $name =~ ^[A-Za-z0-9._-]+$ ]] || usage
[[ -f build.sh && -x tools/macos/heavy.sh ]] || { echo 'preview: run from a tree root (build.sh, tools/macos/heavy.sh)' >&2; exit 2; }
[[ -d $stage ]] || { echo "preview: stage directory $stage does not exist" >&2; exit 2; }
say() { echo "preview: $name: $*"; echo "$*" >> "$facts"; }
die() { echo "preview: $name: FAILED: $*" >&2; [[ -n ${facts:-} && -e $facts ]] && echo "FAILED: $*" >> "$facts"; exit 1; }

if [[ -n $(git diff HEAD --ignore-submodules=dirty --stat) ]]; then
    echo "preview: $name: refused: tracked files differ from HEAD (a preview is a commit)" >&2; exit 2
fi
git submodule update -q
head=$(git rev-parse --short HEAD)
dest=$stage/$head-$name
[[ ! -e $dest ]] || { echo "preview: $name: refused: $dest exists (a preview is never overwritten)" >&2; exit 2; }
mkdir -p out "$dest/gpu"
facts=$dest/PREVIEW.txt
: > "$facts"
say "commit $(git log --oneline -1 | cut -c1-100)"
say "tree $(git rev-parse --short 'HEAD^{tree}')"
export BB_DEPS_PREFIX=${BB_DEPS_PREFIX:-$HOME/bbport-deps/x86_64}

started=$(date +%s)
BB_HEAVY_PRIORITY=$priority bash build.sh > "out/preview-$name-build.log" 2>&1 ||
    die "build (log: out/preview-$name-build.log): $(tail -3 "out/preview-$name-build.log" | tr '\n' ' ')"
say "built in $(( $(date +%s) - started )) s"
while read -r line; do say "$line"; done < <(grep -E '^GPU library: ccache|host imports verified' "out/preview-$name-build.log")

cp out/bb-probe "$dest/" && cp out/gpu/libbbgpu.dylib "$dest/gpu/" || die "staging to $dest"
(cd "$dest" && shasum -a 256 bb-probe gpu/libbbgpu.dylib > SHA256SUMS) || die "hashing"
codesign -v "$dest/bb-probe" "$dest/gpu/libbbgpu.dylib" || die "a staged binary's signature does not verify"
say "staged $dest"
while read -r sum file; do say "sha256 $file $sum"; done < "$dest/SHA256SUMS"
say "signatures: codesign -v OK"
for version in $(grep -ho 'ShaderBinaryVersion = [0-9]*u' gpu/shadps4/video_core/renderer_vulkan/*.cpp 2>/dev/null | grep -o '[0-9]*'); do
    say "shader binary version $version"
done

# heavy.sh's exit, and the probe's own last line (heavy's lines, e.g. "PAUSED ... for perf runs" when a
# perf run paused the smoke job, come after it).
VK_DRIVER_FILES=$BB_DEPS_PREFIX/share/vulkan/icd.d/MoltenVK_icd.json \
    tools/macos/heavy.sh --pool quick "$name smoke" -- "$dest/bb-probe" --vulkan-only > "out/preview-$name-smoke.log" 2>&1
smoke_status=$?
smoke=$(grep -v '^heavy: ' "out/preview-$name-smoke.log" | tail -1)
[[ $smoke_status == 0 && $smoke == *PASS* ]] || die "smoke (exit $smoke_status): $(tail -3 "out/preview-$name-smoke.log" | tr '\n' ' ')"
say "smoke: $smoke"
if [[ -n $build_only ]]; then say "test: not run (--build-only)"; exit 0; fi

started=$(date +%s)
BB_HEAVY_PRIORITY=$priority BB_TEST_SKIP=$skip bash build.sh --test-keep-going > "out/preview-$name-test.log" 2>&1
status=$?
summary=$(grep '^test-keep-going: [0-9]' "out/preview-$name-test.log" | tail -1)
case $status in
0) verdict=PASS ;;
3) verdict='NOT A FULL GATE (skips)' ;;
*) verdict=FAILED ;;
esac
say "test: $verdict (build.sh --test-keep-going exit $status, $(( $(date +%s) - started )) s): ${summary:-no summary: a build or non-test step failed, see out/preview-$name-test.log}"
while read -r line; do say "test ${line#  }"; done < <(grep -E '^  (FAILED|skipped): ' "out/preview-$name-test.log")
# Paused or demoted (E-core QoS) by a perf run while it ran? heavy.sh's history has the build.sh
# job's quiet-pause / quiet-ecore events; either can make a time limit in a test fire.
history=${BB_HEAVY_DIR:-$HOME/bbport-deps/locks}/history.jsonl
job="build.sh --test-keep-going in $(basename "$(dirname "$PWD")")/$(basename "$PWD")"
paused=$(python3 - "$history" "$job" "$started" <<'PY'
import datetime, json, os, sys
path, job, since = sys.argv[1], sys.argv[2], float(sys.argv[3])
count = 0
if os.path.exists(path):
    for line in open(path):
        event = json.loads(line)
        if event.get('event') in ('quiet-pause', 'quiet-ecore') and event.get('label') == job and \
                datetime.datetime.fromisoformat(event['time']).timestamp() >= since - 1:
            count += 1
print(count)
PY
)
say "test paused or demoted by perf runs: $paused time(s)"
if [[ $status == 1 && $paused != 0 ]]; then
    grep -E '^  FAILED: exit [0-9]+: ' "out/preview-$name-test.log" | sed -E 's/^  FAILED: exit [0-9]+: //' |
    while read -r command; do
        if [[ $command == spirv-val* ]]; then  # a check of a test's output; its test is listed too
            say "rerun: not for $command (rerun the test that wrote it)"; continue
        fi
        one=$(for word in $command; do [[ $word == */* ]] && { echo "$word"; break; }; done)
        [[ -n $one ]] || one=${command%% *}
        log=out/preview-$name-rerun-$(basename "$one").log
        BB_HEAVY_PRIORITY=$priority bash build.sh --test-one "$one" > "$log" 2>&1
        say "rerun (paused run) --test-one $one: exit $?: $(grep -E '^test-one: ' "$log" | tail -1)"
    done
fi
exit $status
