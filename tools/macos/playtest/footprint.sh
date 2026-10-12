#!/bin/bash
# footprint.sh PROBE LABEL FIXTURE SECONDS [walk] : BB_PASS_FOOTPRINT_VERIFY=1 run (Sol-2's real-draw coverage
# check, PREVIEW-27h). Correctness builds: expect no stop and a census line. Mutation controls (27h-mi/-mv):
# expect a loud stop naming the index buffer / vertex input; no stop means the verifier is not independent.
# "walk" walks and turns between captures. Env: WARM (default p23-clinic).
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/../../.." && pwd); cd "$ROOT"
P=$1; L=$2; F=$3; S=$4; W=$5
[ -n "$S" ] || { echo "usage: footprint.sh PROBE LABEL FIXTURE SECONDS [walk]"; exit 2; }
timeout 900 python3 tools/macos/play_route.py route --probe $P --label $L --fps 60 --gpu-log info --fixture $F \
  --warm-cache ${WARM:-p23-clinic} --diag-env BB_PASS_FOOTPRINT_VERIFY=1 --until world --keep 2>&1 | grep 'play_route' | grep -v 'not passed\|still waiting' | cut -c1-200
R=$(ls -td out/macos_route/*-$L | head -1); echo "RUN $R"
t(){ python3 tools/macos/play_route.py --run $R "$@" >/dev/null 2>&1; }
grep -m3 -i "footprint" $R/game.log | cut -c1-300
w=$(date +%s); i=0
while [ ! -f $R/exit.json ] && [ $(( $(date +%s) - w )) -lt $S ]; do
  i=$((i+1))
  if [ "$W" = walk ]; then case $((i % 4)) in 1) t press ly=0 4.0;; 2) t press rx=255 1.5;; 3) t press ly=0 3.0;; 0) t press rx=0 2.0;; esac; else sleep 10; fi
  t capture s$i
done
if [ -f $R/exit.json ]; then
  echo "STOPPED after $(( $(date +%s) - w )) s in the world: $(cat $R/exit.json)"
  l=$(grep -n -m1 "Assertion Failed\|^STOP\|Critical" $R/game.log | cut -d: -f1)
  [ -n "$l" ] && sed -n "$((l>3 ? l-3 : 1)),$((l+4))p" $R/game.log | cut -c1-2000
else
  echo "NO STOP in $S s"; t stop
fi
echo "-- footprint lines: $(grep -ci 'footprint' $R/game.log)"; grep -i "footprint" $R/game.log | tail -4 | cut -c1-600
python3 out/macos_route/detector/score.py $R 2>/dev/null | head -2
