#!/bin/bash
# Usage: bash tools/macos/replay-ab.sh [options] A_REF B_REF
# Perf replay A/B (docs/macos/capture-replay.md): each ref, merged with the replay harness when it
# lacks it, is checked out in its own scratch worktree and built there (bash build.sh in a heavy.sh
# build slot); then the arms replay one capture interleaved (--order), each run in a heavy.sh
# playtest slot with the GPU exclusive and CPU quiet (stop), the host state recorded before it,
# and a watchdog. Prints the per-run summary and the per-arm table (replay_summary.py --arms).
# Options:
#   --iters N        iterations per run (default 100; iteration 1 is warm-up)
#   --order ABBA     run order, letters A and B (default ABBA)
#   --env "K=V ..."  environment for both arms; --env-a / --env-b for one arm only
#   --harness REF    merged into a ref without gpu/shim/bbport_replay.cpp (default macos)
#   --capture DIR    capture to replay (default: the newest out/replay/frame-* of this tree)
#   --data DIR       boot data (default out/macos_route/data-1080p of this tree, as play_route makes)
#   --label NAME     names the scratch worktrees, logs and heavy jobs (default replay-ab-<time>)
#   --keep           keep the scratch worktrees (default: removed after the runs)
# Scratch worktrees: ${BB_REPLAY_AB_DIR:-$HOME/.cache/bbport-replay-ab}/<label>-a|-b. Logs: the
# label's directory under out/replay-ab/. State the power mode (printed) with any timing.
set -uo pipefail
die() { echo "STOP: replay-ab: $*" >&2; exit 2; }
[[ -f build.sh && -f tools/macos/heavy.sh ]] || die "run from a tree root"
ROOT=$PWD
iters=100 order=ABBA env_both="" env_a="" env_b="" harness=macos capture="" data="" keep=0
label=replay-ab-$(date +%Y%m%d-%H%M%S)
refs=()
while (($#)); do
    case $1 in
    --iters) iters=${2:?}; shift 2 ;;
    --order) order=${2:?}; shift 2 ;;
    --env) env_both=${2:?}; shift 2 ;;
    --env-a) env_a=${2:?}; shift 2 ;;
    --env-b) env_b=${2:?}; shift 2 ;;
    --harness) harness=${2:?}; shift 2 ;;
    --capture) capture=${2:?}; shift 2 ;;
    --data) data=${2:?}; shift 2 ;;
    --label) label=${2:?}; shift 2 ;;
    --keep) keep=1; shift ;;
    -h|--help) sed -n '2,23p' "$0"; exit 0 ;;
    -*) die "unknown option $1" ;;
    *) refs+=("$1"); shift ;;
    esac
done
((${#refs[@]} == 2)) || die "two refs needed (A_REF B_REF); --help for the options"
[[ $iters =~ ^[0-9]+$ ]] && ((iters >= 2 && iters <= 10000)) || die "--iters $iters: 2..10000"
[[ $order =~ ^[AB]+$ ]] || die "--order $order: letters A and B only"
[[ $label =~ ^[A-Za-z0-9._-]+$ ]] || die "--label $label: letters, digits, . _ - only"
if [[ -z $capture ]]; then
    capture=$(ls -d "$ROOT"/out/replay/frame-* 2>/dev/null | grep -v '\.incomplete$' | sort | tail -1)
fi
[[ -n $capture && -f $capture/manifest.json ]] || die "no complete capture (--capture DIR)"
data=${data:-$ROOT/out/macos_route/data-1080p}
for f in boot-linked.bin content.bin patches.bin; do
    [[ -f $data/out/$f ]] || die "$data/out/$f missing (--data: play_route's data directory)"
done
game=${BB_GAME_DIR:-$HOME/CUSA03173}
deps=${BB_DEPS_PREFIX:-$HOME/bbport-deps/x86_64}
[[ -d $game && -f $deps/share/vulkan/icd.d/MoltenVK_icd.json ]] || die "game ($game) or deps ($deps) missing"
base=${BB_REPLAY_AB_DIR:-$HOME/.cache/bbport-replay-ab}
logs=$ROOT/out/replay-ab/$label
mkdir -p "$base" "$logs" || die "cannot create $base or $logs"

# --- scratch worktrees and builds -------------------------------------------------------------
cleanup() {
    ((keep)) && return
    for x in a b; do
        [[ -d $base/$label-$x ]] && git worktree remove --force "$base/$label-$x" >/dev/null 2>&1
    done
}
trap cleanup EXIT
arm_dir() { echo "$base/$label-$(echo "$1" | tr AB ab)"; }
for X in A B; do
    ref=${refs[$([[ $X == A ]] && echo 0 || echo 1)]}
    dir=$(arm_dir $X)
    commit=$(git rev-parse --verify -q "$ref^{commit}") || die "arm $X: $ref is not a commit"
    [[ -e $dir ]] && die "$dir exists (another run with label $label?)"
    git worktree add -q --detach "$dir" "$commit" || die "arm $X: git worktree add failed"
    if ! git -C "$dir" cat-file -e HEAD:gpu/shim/bbport_replay.cpp 2>/dev/null; then
        git -C "$dir" -c user.name=replay-ab -c user.email=replay-ab@localhost \
            merge -q --no-edit "$harness" >/dev/null 2>&1 ||
            die "arm $X: $ref + harness $harness does not merge cleanly: $(git -C "$dir" diff --name-only --diff-filter=U | tr '\n' ' ')"
    fi
    # Submodules come from this tree's checkouts (no network; build.sh only checks and patches).
    for sub in $(git config -f .gitmodules --get-regexp 'submodule\..*\.path' | awk '{print $2}'); do
        [[ -d $ROOT/$sub ]] && rsync -a --delete "$ROOT/$sub/" "$dir/$sub/"
    done
    echo "arm $X: $ref -> $(git -C "$dir" log -1 --format=%h) in $dir"
    (cd "$dir" && bash tools/macos/heavy.sh --pool build "$label build $X" -- bash build.sh) \
        > "$logs/build-$X.log" 2>&1 || die "arm $X: build failed (see $logs/build-$X.log)"
    [[ -x $dir/out/bb-probe && -f $dir/out/gpu/libbbgpu.dylib ]] || die "arm $X: no bb-probe or libbbgpu"
done

# --- interleaved runs ---------------------------------------------------------------------------
ini=$logs/bbport.ini
printf 'preset=0\noutput_res=1920x1080\nupscaler=off\nobject_motion=0\nlive_resolution=0\n' > "$ini"
mkdir -p "$logs/user"
n=0
for X in $(echo "$order" | fold -w1); do
    n=$((n + 1)); dir=$(arm_dir $X); tag=$n-$X
    [[ $X == A ]] && extra=$env_a || extra=$env_b
    log=$logs/run-$tag.log
    {
        echo "== host before run $tag ($(date -u +%FT%TZ))"
        echo "powermode: $(pmset -g custom | awk '/powermode/{print $2}' | tr '\n' ' ')"
        echo "load: $(sysctl -n vm.loadavg)"
        pmset -g therm | sed 's/^/therm: /'
        memory_pressure -Q 2>/dev/null | sed 's/^/memory: /'
        ps -Ao pcpu,pid,comm -r | head -6 | sed 's/^/top: /'
    } > "$logs/host-$tag.txt"
    # shellcheck disable=SC2086 # the env words split on purpose
    bash tools/macos/heavy.sh --pool playtest --gpu exclusive --cpu-quiet "$label run $tag" -- \
        python3 tools/macos/replay_watch.py "$log" 60 300 -- env \
        VK_DRIVER_FILES="$deps/share/vulkan/icd.d/MoltenVK_icd.json" BB_CONFIG="$ini" BB_UPSCALER=none \
        BB_WINDOW_SIZE=320x180 BB_WINDOW_BACKGROUND=1 BB_WINDOW_POS=0,0 BB_FRAME_STATS=1 BB_GPU_LOG=info \
        BB_REPLAY_GPU_WAIT=1 $env_both $extra BB_REPLAY_RUN="$capture" BB_REPLAY_ITERATIONS="$iters" \
        "$dir/out/bb-probe" "$data/out/boot-linked.bin" --content-profile "$data/out/content.bin" \
        --patches "$data/out/patches.bin" --app0 "$game" --user "$logs/user" --timeout 900 \
        2> "$logs/heavy-$tag.err"
    echo "run $tag (arm $X, $(git -C "$dir" log -1 --format=%h)): rc $?"
    grep -E "CPU quiet|INVALID" "$logs/heavy-$tag.err" | sed 's/^/  /'
    grep -m2 -E "^STOP|WATCHDOG" "$log" | sed 's/^/  /'
done
echo "capture $capture; logs $logs"
python3 tools/macos/replay_summary.py --arms "$logs"/run-*.log
