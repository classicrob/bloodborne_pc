#!/bin/bash
# arch-route.sh PROBE LABEL [play_route args...] : the Central Yharnam coffin-arch route from fixture
# central-yharnam-arch (the depth-bounds stop repro, BindDepthBoundsSource; fixed in 12365a4). Timed presses
# drift with FPS: if the arch is not reached, walk the last part by eye. KEEP=1 leaves the game running.
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/../../.." && pwd); cd "$ROOT"
P=$1; L=$2; shift 2
[ -n "$L" ] || { echo "usage: arch-route.sh PROBE LABEL [play_route args...]"; exit 2; }
timeout 900 python3 tools/macos/play_route.py route --probe $P --label $L --fps 60 --gpu-log info --fixture central-yharnam-arch \
  --warm-cache ${WARM:-p23-clinic} "$@" --until world --keep 2>&1 | grep 'play_route' | grep -v 'not passed\|still waiting' | cut -c1-200
R=$(ls -td out/macos_route/*-$L | head -1)
t(){ python3 tools/macos/play_route.py --run $R "$@" >/dev/null 2>&1; }
for s in "rx=255 1.2" "rx=255 1.2" "rx=0 0.6" "ly=0 5.0"; do t press $s; done; t capture at-arch
t press rx=255 1.2; t capture turned
w=$(date +%s); while [ ! -f $R/exit.json ] && [ $(( $(date +%s) - w )) -lt 40 ]; do sleep 2; done
if [ -f $R/exit.json ]; then echo "STOPPED: $(cat $R/exit.json)"; grep -B12 -A2 -m1 "Assertion Failed" $R/game.log | cut -c1-300
else echo "no stop within 40 s of the turn"; [ "${KEEP:-0}" = 1 ] || t stop; fi
echo "RUN $R"
