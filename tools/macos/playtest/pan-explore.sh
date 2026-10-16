#!/bin/bash
# pan-explore.sh PROBE LABEL FIXTURE LEGS [TURN_S] : rendering coverage. Loads FIXTURE, then LEGS times: walk
# forward 4 s, capture, pan the camera in four 90-degree-ish steps (rx=255 for TURN_S, default 1.3) capturing
# each, then turn back toward the path. Stops when the game exits (a STOP is printed verbatim). Prints a
# contact sheet path per leg (out/macos_route/<run>/sheet-NN.jpg) for review by eye.
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/../../.." && pwd); cd "$ROOT"
P=$1; L=$2; F=$3; N=$4; TS=${5:-1.3}
[ -n "$N" ] || { echo "usage: pan-explore.sh PROBE LABEL FIXTURE LEGS [TURN_S]"; exit 2; }
timeout 900 python3 tools/macos/play_route.py route --probe $P --label $L --fps 60 --gpu-log info --fixture $F \
  --warm-cache ${WARM:-p23-clinic} ${EXTRA} --until world --keep 2>&1 | grep 'play_route' | grep -v 'not passed\|still waiting' | cut -c1-200
R=$(ls -td out/macos_route/*-$L | head -1); echo "RUN $R"
t(){ python3 tools/macos/play_route.py --run $R "$@" >/dev/null 2>&1; }
alive(){ [ ! -f $R/exit.json ]; }
for leg in $(seq 1 $N); do
  alive || break
  t press ly=0 4.0; t capture L$leg-walk
  for k in 1 2 3 4; do alive || break; t press rx=255 $TS; t capture L$leg-pan$k; done
  t press rx=0 $(python3 -c "print(round($TS*1.5,2))")
  python3 - "$R" "$leg" <<'PY'
import sys
from pathlib import Path
from PIL import Image
run, leg = Path(sys.argv[1]), sys.argv[2]
ps = [p for n in ('walk', 'pan1', 'pan2', 'pan3', 'pan4') for p in sorted((run / 'shots').glob(f'*-L{leg}-{n}.jpg'))[-1:]]
s = Image.new('RGB', (1512, 450 * ((len(ps) + 1) // 2)))
for i, p in enumerate(ps):
    s.paste(Image.open(p).convert('RGB').resize((756, 450)), ((i % 2) * 756, (i // 2) * 450))
s.save(run / f'sheet-{int(leg):02d}.jpg', quality=82)
print(f'sheet {run}/sheet-{int(leg):02d}.jpg')
PY
done
if [ -f $R/exit.json ]; then
  echo "EXITED: $(cat $R/exit.json)"; grep -B3 -A2 -m1 "Assertion Failed\|^STOP" $R/game.log | cut -c1-600
else t stop; echo "stopped by the script"; fi
