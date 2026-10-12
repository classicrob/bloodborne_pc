#!/bin/bash
# motion.sh PROBE : camera vs object motion-vector agreement gate (Opus-Renderer). Env: WARM (default p23-clinic).
# clinic-start idle pose, upscaler on (default), BB_MOTION_CAMERA_ONLY=1; two segments (stick up, stick
# down), each dumping 8 frames into its own directory; tools/macos/motion_agree.py judges each.
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/../../.." && pwd); cd "$ROOT"
B=$PWD/out/macos_route/motion-$(date +%H%M%S); mkdir -p $B; ROUTE=$(mktemp)
python3 tools/macos/play_route.py route --probe $1 --label MOTION --parallel --fps 60 --gpu-log info --fixture clinic-start \
  --warm-cache ${WARM:-p23-clinic} --diag-env BB_MOTION_CAMERA_ONLY=1 --diag-env BB_DUMP_TRIGGER=$B/dump.trigger \
  --diag-env BB_DUMP_DIR=$B/dump --diag-env BB_DUMP_FRAMES=8 --until world --keep 2>&1 | grep 'play_route' | grep -v 'not passed\|still waiting' | tee $ROUTE
R=$(ls -td out/macos_route/*MOTION | head -1)
grep -q 'timed out\|fatal\|exited' $ROUTE && { echo "ROUTE FAILED"; python3 tools/macos/play_route.py --run $R stop >/dev/null 2>&1; exit 1; }
grep -m1 "Object motion:" $R/game.log; grep -m1 "BB_MOTION_CAMERA_ONLY=1" $R/game.log
grep -q "Object motion: on" $R/game.log && grep -q "BB_MOTION_CAMERA_ONLY=1: motion vectors are camera-only" $R/game.log \
  || { echo "STOP: object motion off or camera-only switch not confirmed"; python3 tools/macos/play_route.py --run $R stop >/dev/null 2>&1; exit 1; }
grep -m1 "^Dump: BB_DUMP_TRIGGER=" $R/game.log || { echo "STOP: no 'Dump: BB_DUMP_TRIGGER=... is polled' line (build without f16186d: the trigger would never fire)"; python3 tools/macos/play_route.py --run $R stop >/dev/null 2>&1; exit 1; }
t(){ python3 tools/macos/play_route.py --run $R "$@" > /dev/null 2>&1; }
for seg in up down; do
  [ $seg = up ] && stick=ry=60 || stick=ry=196
  before=$(( $(wc -l < $R/game.log) ))
  ( sleep 1.0; touch $B/dump.trigger ) & t press "$stick" 8.0; wait
  dir=""; for i in $(seq 1 60); do dir=$(tail -n +$((before + 1)) $R/game.log | grep -m1 -o 'Dump: 8 frames to [^ ]*' | sed 's/Dump: 8 frames to //'); [ -n "$dir" ] && break; sleep 1; done
  sleep 8
  echo "segment $seg: session ${dir:-NONE}"
  if [ -n "$dir" ]; then python3 tools/macos/motion_agree.py "$dir"; code=$?; else code=none; fi
  echo "segment $seg exit $code (0 agree, 1 disagree, 2 nothing judged, 3 unusable)"
  sleep 3
done
python3 tools/macos/play_route.py --run $R stop > /dev/null 2>&1
echo "RUN $R DUMPS $B"
