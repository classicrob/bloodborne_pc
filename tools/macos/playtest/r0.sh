#!/bin/bash
# r0.sh PROBE : quiet R0 perf run (Opus-Playtest). Runs alone: heavy.sh --gpu exclusive --cpu-quiet.
# clinic-start -> world (--fps 60, labels at GPU completion, flips at retire), 5 min idle at the pose
# (5 captures), 5 min fixed walk loop; prints FPS, Submit pacing, Parse lag, Render passes and the
# Parse split blocks per segment. Env: WARM (cache fixture, default p23-clinic), R0_LABEL, R0_EXTRA
# (extra play_route args, e.g. "--perf"), R0_FLIP_AT/R0_FLIP_VALUE[/R0_FLIP_VALUE2] (live toggle flip
# that many seconds into the idle segment; needs R0_EXTRA="--disable-optimizations 0").
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/../../.." && pwd)
[ -n "$1" ] || { echo "usage: r0.sh PROBE"; exit 2; }     # before heavy.sh: no slot or GPU wait for a bad call
[ -x "$1" ] || { echo "r0.sh: no executable probe at $1"; exit 2; }
if [ "$BB_HEAVY_GPU" != "exclusive" ]; then
  cd "$ROOT" && exec tools/macos/heavy.sh --pool playtest --gpu exclusive --cpu-quiet "R0 perf (${R0_LABEL:-R0-perf})" -- /bin/bash "$HERE/r0.sh" "$@"
fi
cd "$ROOT"
# Host state per run (orchestrator): thermal and memory pressure before the route and after the walk,
# kept in the run dir as host.txt. The load wait and the CPU-hog check are heavy.sh's (--settle).
host(){ echo "## $1 $(date -u +%T): load $(sysctl -n vm.loadavg)"; pmset -g therm 2>&1 | grep -v '^$'; memory_pressure -Q 2>&1 | tail -1; }
HOST_BEFORE=$(host before)
L=${R0_LABEL:-R0-perf}; ROUTE=$(mktemp)
python3 tools/macos/play_route.py route --probe "$1" --label $L --fps 60 --gpu-log info --fixture clinic-start \
  --warm-cache ${WARM:-p23-clinic} --diag-env BB_LABELS_AT_GPU_COMPLETE=1 --diag-env BB_FLIP_AT_GPU_RETIRE=1 ${R0_EXTRA} \
  --until world --keep 2>&1 | grep 'play_route' | grep -v 'not passed\|still waiting' | cut -c1-200 | tee $ROUTE
R=$(ls -td out/macos_route/*$L | head -1)
grep -q 'timed out\|fatal\|exited' $ROUTE && { echo "ROUTE FAILED"; python3 tools/macos/play_route.py --run $R stop >/dev/null 2>&1; exit 1; }
rm -f $ROUTE
t(){ python3 tools/macos/play_route.py --run $R "$@" > /dev/null 2>&1; }
mark(){ echo "$1 line=$(( $(wc -l < $R/game.log) )) $(date -u +%T) perf=$(( $(cat $R/perf.jsonl 2>/dev/null | wc -l) ))"; }
echo "$HOST_BEFORE" > $R/host.txt
grep -m3 "Labels (EOP/EOS/RELEASE_MEM)\|Flips are published\|App Nap" $R/game.log | cut -c1-140
mark idle-start > $R/segments.log
if [ -n "$R0_FLIP_AT" ]; then
  [ -f $R/toggles ] || { echo "R0_FLIP_AT set but the run has no toggle file (R0_EXTRA needs --disable-optimizations 0)"; t stop; exit 1; }
  ( sleep $R0_FLIP_AT; printf "%s %s\n" "$R0_FLIP_VALUE" "${R0_FLIP_VALUE2:-0}" > $R/toggles.tmp && mv $R/toggles.tmp $R/toggles; mark flip >> $R/segments.log ) &
fi
for c in 1 2 3 4 5; do sleep 55; t capture idle-$c; done
mark walk-start >> $R/segments.log
end=$(( $(date +%s) + 300 ))
while [ $(date +%s) -lt $end ] && [ ! -f $R/exit.json ]; do
  t press "ly=0" 6.0; t press "lx=255" 2.0; t press "ly=0" 6.0; t press "lx=0" 2.0; t press "rx=230" 1.5; t press "ly=255" 5.0
  wl=$((${wl:-0} + 1)); t capture walk-$wl
done
mark end >> $R/segments.log
host after >> $R/host.txt
cat $R/segments.log; cat $R/host.txt
python3 - "$R" <<'PY'
import re, sys
run = sys.argv[1]
marks = [(s[0], int(s[1].split('=')[1])) for s in (l.split() for l in open(run + '/segments.log'))]
lines = open(run + '/game.log', errors='replace').read().splitlines()
for (name, a), (_, b) in zip(marks, marks[1:]):
    part = lines[a:b]
    st = [re.match(r'Frame stats: ([\d.]+) FPS, worst frame ([\d.]+)', l) for l in part]
    fps = [float(m.group(1)) for m in st if m]; worst = [float(m.group(2)) for m in st if m]
    print(f'== {name}: {len(fps)} frame-stats windows, mean FPS {sum(fps)/max(1,len(fps)):.2f}, '
          f'mean frame {1000*len(fps)/max(0.01,sum(fps)):.0f} ms, worst frame max {max(worst or [0]):.0f} ms')
    for i, l in enumerate(part):
        if l.startswith(('GpuIdle split', 'Submit pacing', 'Parse lag', 'Render passes', 'Flush')):
            print('  ' + l)
        elif l.startswith('Parse split'):
            for x in part[i:i + 8]:  # header, stage A (+2), stage B, Vulkan recording thread, Flushes line
                print('  ' + x)
PY
python3 tools/macos/play_route.py --run $R report | python3 -c "import json,sys;d=json.load(sys.stdin);print({k:v for k,v in d['counts'].items() if v}, d['exit'], d['stop'])"
t stop
echo "RUN $R"
