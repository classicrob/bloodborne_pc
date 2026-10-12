#!/bin/bash
# perfcap.sh PROBE REPLAY_DIR : the verifier's live perf capture (replay "perf:60") at the settled clinic
# idle: upscaler off, no object motion, no live resolution; trigger after 20 s, HOLD 3000 ms.
# Env: WARM (default p23-clinic; builds with Apple shader cache 31), LABEL (default PERFCAP).
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/../../.." && pwd); cd "$ROOT"
[ -n "$2" ] || { echo "usage: perfcap.sh PROBE REPLAY_DIR"; exit 2; }
L=${LABEL:-PERFCAP}; TRIG=$ROOT/out/macos_route/replay-$(date +%H%M%S).trigger; ROUTE=$(mktemp)
python3 tools/macos/play_route.py route --probe $1 --label $L --parallel --fps 60 --gpu-log info --fixture clinic-start \
  --warm-cache ${WARM:-p23-clinic} --ini upscaler=off --ini object_motion=0 --ini live_resolution=0 \
  --diag-env BB_UPSCALER=none --diag-env BB_REPLAY_TRIGGER=$TRIG --diag-env BB_REPLAY_CAPTURE=perf:60 \
  --diag-env BB_REPLAY_DIR=$2 --diag-env BB_REPLAY_HOLD_MS=3000 --until world --keep 2>&1 \
  | grep 'play_route' | grep -v 'not passed\|still waiting' | tee $ROUTE
R=$(ls -td out/macos_route/*-$L | head -1)
grep -q 'timed out\|fatal\|exited' $ROUTE && { echo "ROUTE FAILED"; grep -m3 "Replay capture\|refus" $R/game.log; python3 tools/macos/play_route.py --run $R stop >/dev/null 2>&1; exit 1; }
t(){ python3 tools/macos/play_route.py --run $R "$@" > /dev/null 2>&1; }
sleep 20; t capture settled-1; sleep 3; t capture settled-2
before=$(( $(wc -l < $R/game.log) )); touch $TRIG; echo "trigger touched $(date -u +%T)"
for i in $(seq 1 300); do tail -n +$before $R/game.log | grep -q "released frame retired\|PERF CAPTURE" && break; [ -f $R/exit.json ] && break; sleep 1; done
sleep 5; t capture after-1
tail -n +$before $R/game.log | grep -v "^    image " | awk '/^Replay capture/ {show=1; print; next} show && /^[ \t]/ {print; next} {show=0}' | head -80
tail -n +$before $R/game.log | grep "released frame retired\|PERF CAPTURE"
cat $R/exit.json 2>/dev/null; t stop; echo "RUN $R"
