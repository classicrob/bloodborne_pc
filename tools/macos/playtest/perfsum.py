"""perfsum.py RUN_DIR : BB_PERF window records per R0 segment (segments.log perf= line marks).
Per segment: busy_union_ms_per_flip, main_ms/flip, main_submissions/flip, flips/window, FPS = flips/seconds
(mean and range), next to the parse-split frame ms of the same segment."""
import json, re, sys
from pathlib import Path
run = Path(sys.argv[1])
recs = [json.loads(l) for l in (run / 'perf.jsonl').read_text().splitlines() if l.strip()]
schema = next((r for r in recs if r.get('type') == 'schema'), {})
marks = []
for l in (run / 'segments.log').read_text().splitlines():
    m = re.match(r'(\S+) line=(\d+) \S+ perf=(\d+)', l)
    if m: marks.append((m.group(1), int(m.group(2)), int(m.group(3))))
lines = (run / 'game.log').read_text(errors='replace').splitlines()
print(f'schema {schema}; perturbed windows: {sum(1 for r in recs if r.get("type") == "window" and r.get("perturbed"))}')
rng = lambda v: f'{sum(v)/len(v):.2f} [{min(v):.2f}..{max(v):.2f}]' if v else 'n/a'
for (name, a, pa), (_, b, pb) in zip(marks, marks[1:]):
    w = [r for r in recs[pa:pb] if r.get('type') == 'window' and r.get('flips')]
    busy = [r['gpu']['busy_union_ms_per_flip'] for r in w]
    main = [r['gpu']['main_ms'] / r['flips'] for r in w]
    subs = [r['gpu']['main_submissions'] / r['flips'] for r in w]
    flips = [r['flips'] for r in w]
    fps = [r['flips'] / r['seconds'] for r in w]
    split = [float(m.group(1)) for m in (re.search(r'Parse split, last 600 frame ends: ([\d.]+) ms per frame', l) for l in lines[a:b]) if m]
    print(f'== {name}: {len(w)} windows')
    print(f'   busy_union_ms_per_flip {rng(busy)}; main_ms/flip {rng(main)}; main_submissions/flip {rng(subs)}')
    print(f'   flips/window {rng(flips)}; FPS (flips/s) {rng(fps)}; frame ms from FPS {1000/(sum(fps)/len(fps)):.1f}' if fps else '   no windows')
    print(f'   parse-split frame ms {rng(split)}')
