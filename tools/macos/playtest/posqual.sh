#!/bin/bash
# posqual.sh PROBE LABEL : observation-only qualification of Astra-2's player-position publisher (BB_PLAYER_POS_FILE).
# From central-yharnam-after-first-death (loads at the edited lamp-yard spawn, the lamp lit): spawn samples, a
# walk, a player turn (left stick sideways: player_yaw changes), a camera turn (right stick: position and
# player_yaw stay), a walk to the lamp, its "Return to the Hunter's Dream" (load + map change: expect invalid
# samples, then a new map_raw) and a walk in the Dream. A sampler appends every new sequence to <run>/positions.jsonl with the host time; the summary
# pairs each capture with the newest sample before it.
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/../../.." && pwd); cd "$ROOT"
P=$1; L=$2; [ -n "$L" ] || { echo "usage: posqual.sh PROBE LABEL"; exit 2; }
POS=$ROOT/out/macos_route/pos-$L-$(date +%H%M%S).json
timeout 900 python3 tools/macos/play_route.py route --probe $P --label $L --fps 60 --gpu-log info \
  --fixture central-yharnam-after-first-death --warm-cache ${WARM:-p23-clinic} --diag-env BB_PLAYER_POS_FILE=$POS \
  --until world --keep 2>&1 | grep 'play_route' | grep -v 'not passed\|still waiting' | cut -c1-200
R=$(ls -td out/macos_route/*-$L | head -1); echo "RUN $R; position file $POS"
t(){ python3 tools/macos/play_route.py --run $R "$@" >/dev/null 2>&1; }
python3 - "$POS" "$R/positions.jsonl" "$R/exit.json" <<'PY' &
import json, os, sys, time
src, out, done = sys.argv[1:4]; last = None
with open(out, 'a') as f:
    while not os.path.exists(done):
        try:
            rec = json.loads(open(src).read())
        except (OSError, ValueError):
            rec = None
        if rec and (last is None or (rec.get('sequence'), rec.get('run_id')) != last):
            last = (rec.get('sequence'), rec.get('run_id')); rec['host_time'] = time.time()
            f.write(json.dumps(rec) + '\n'); f.flush()
        time.sleep(0.05)
PY
SAMPLER=$!
mark(){ echo "{\"step\": \"$1\", \"host_time\": $(python3 -c 'import time;print(time.time())')}" >> $R/steps.jsonl; t capture $1; }
sleep 5; mark spawn
# to the Central Yharnam lamp (right of the start, by the carved doors; the CYHARNAM-1 moves): the walk sample
t press lx=255 1.2; t press ly=0 1.5; sleep 2; mark at-lamp
t press lx=0 0.6; sleep 1; mark player-turn
t press rx=255 1.5; sleep 1; mark camera-turn
t press lx=255 0.6; sleep 1; mark back-at-lamp
t press cross; sleep 3; mark lamp-prompt; t press cross; sleep 3; mark after-confirm
sleep 45; mark dream-arrival
t press ly=0 3.0; sleep 1; mark dream-walk
sleep 3; mark end
t stop; sleep 1; kill $SAMPLER 2>/dev/null
python3 - "$R" <<'PY'
import json, math, sys
from pathlib import Path
run = Path(sys.argv[1])
samples = [json.loads(l) for l in (run / 'positions.jsonl').read_text().splitlines() if l.strip()]
steps = [json.loads(l) for l in (run / 'steps.jsonl').read_text().splitlines() if l.strip()]
print(f'samples: {len(samples)}; valid {sum(1 for s in samples if s.get("valid"))}; invalid reasons: '
      f'{sorted(set(str(s.get("reason")) for s in samples if not s.get("valid")))}; maps: {sorted(set(hex(s["map_raw"]) for s in samples if s.get("valid")))}')
seqs = [s['sequence'] for s in samples]
print(f'sequence monotonic: {all(b > a for a, b in zip(seqs, seqs[1:]))}; run_ids: {len(set(s.get("run_id") for s in samples))}; max sample_age_ms {max(s.get("sample_age_ns", 0) for s in samples) / 1e6:.1f}')
prev = None
for st in steps:
    before = [s for s in samples if s['host_time'] <= st['host_time']]
    s = before[-1] if before else None
    if s and s.get('valid'):
        x, y, z = s['xyz']; yaw = s['player_yaw_rad']
        d = f'; moved {math.dist(prev["xyz"], s["xyz"]):.2f}, yaw change {math.degrees(yaw - prev["player_yaw_rad"]):+.1f} deg' if prev and prev.get('valid') and prev['map_raw'] == s['map_raw'] else ''
        print(f'{st["step"]:14s} seq {s["sequence"]:5d} map {hex(s["map_raw"])} xyz ({x:.2f}, {y:.2f}, {z:.2f}) yaw {math.degrees(yaw):7.1f} deg{d}')
    else:
        print(f'{st["step"]:14s} ' + (f'seq {s["sequence"]} INVALID ({s.get("reason")})' if s else 'no sample yet'))
    prev = s
PY
[ -f $R/exit.json ] && cat $R/exit.json; grep -m2 -i "position\|Assertion Failed\|^STOP" $R/game.log | cut -c1-300
