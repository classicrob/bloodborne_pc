#!/usr/bin/env python3
"""Usage: python3 tools/macos/replay_summary.py [--arms] LOG...
Per perf replay log (iterations 2..N): wall median/p90/max, iterations over 75 ms, P-core share,
effective clock, GPU busy, GPU thread CPU, and medians of parse-split fields (on-CPU per thread,
queue submit, deferred signals before WRITE_DATA, DUMP_CONST_RAM drain, flushes, label age,
WRITE_DATA label waits). With --arms, logs named ...-<run>-<ARM>.log are also pooled per arm
(median of the run medians, mean of p90s) into an A/B table."""
import os, re, sys, statistics as st
def p90(v): v=sorted(v); return v[min(len(v)-1, int(round(0.9*(len(v)-1))))]
FIELDS = [('A on CPU', r'stage A \(GPU command thread, [\d.]+%, on CPU ([\d.]+) ms'),
          ('B on CPU', r'stage B \(draw recording thread, [\d.]+%, on CPU ([\d.]+) ms'),
          ('VK on CPU', r'Vulkan recording thread \([\d.]+%, on CPU ([\d.]+) ms'),
          ('B queue submit', r'stage B .*in scopes: .*?queue submit ([\d.]+)'),
          ('VK queue submit', r'Vulkan recording thread .*?queue submit ([\d.]+)'),
          ('B deferred before WRITE_DATA', r'deferred signals before WRITE_DATA ([\d.]+)'),
          ('A drain DUMP_CONST_RAM', r'stage B drain at DUMP_CONST_RAM ([\d.]+)'),
          ('flushes/frame', r'Flushes of the draw scheduler: ([\d.]+) per frame'),
          ('label issue->write us', r'issue to write (\d+) us mean'),
          ('WRITE_DATA label waits/frame', r'waited for labels ([\d.]+)'),
          ('WRITE_DATA wait us', r'waited for labels [\d.]+, (\d+) us mean wait')]
arms = '--arms' in sys.argv
paths = [a for a in sys.argv[1:] if a != '--arms']
if not paths:
    sys.exit(__doc__)
per_arm = {}
for path in paths:
    it=None; rows={}; busy={}; fields={k:{} for k,_ in FIELDS}; block=[]
    for l in open(path, errors='replace'):
        if l.startswith('Parse split'): block=[l]; continue
        if block and l.startswith(' '): block.append(l)
        m=re.match(r'Perf replay iteration (\d+)[^:]*: ([\d.]+) ms submit-to-idle.*?GPU thread CPU ([\d.]+)', l)
        if m:
            it=int(m.group(1)); rows[it]={'wall':float(m.group(2)),'cpu':float(m.group(3))}
            text=''.join(block)
            for k,rx in FIELDS:
                mm=re.search(rx, text)
                if mm: fields[k][it]=float(mm.group(1))
            block=[]; continue
        m=re.match(r'  cores during.*P-core share (\d+)%.*?([\d.]+) GHz.*task role (\d+)', l)
        if m and it: rows[it].update(p=int(m.group(1)), ghz=float(m.group(2)), role=int(m.group(3)))
        m=re.match(r'  GPU busy of iteration (\d+).*: ([\d.]+) ms', l)
        if m: busy[int(m.group(1))]=float(m.group(2))
    its=[i for i in sorted(rows) if i>=2]
    if not its: print(path, ': no iterations'); continue
    w=[rows[i]['wall'] for i in its]; P=[rows[i].get('p',0) for i in its]
    b=[busy[i] for i in its if i in busy]
    print(f"{path.split('/')[-1]}: n={len(its)} wall median {st.median(w):.2f} p90 {p90(w):.2f} max {max(w):.1f} >75ms {sum(x>75 for x in w)} | "
          f"P-core median {st.median(P)}% min {min(P)}% | GHz {st.median(rows[i].get('ghz',0) for i in its):.2f} | "
          f"GPU busy median {st.median(b) if b else 0:.2f} | GPU thread CPU {st.median(rows[i]['cpu'] for i in its):.2f} | roles {sorted({rows[i].get('role') for i in its})}")
    arm = os.path.basename(path).rsplit('.', 1)[0].rsplit('-', 1)[-1]
    per_arm.setdefault(arm, []).append((st.median(w), p90(w), st.median(b) if b else 0.0))
    print('   ' + '; '.join(f"{k} {st.median(v[i] for i in its if i in v):.2f}" for k,v in fields.items() if any(i in v for i in its)))
if arms and len(per_arm) > 1:
    print('\n| arm | runs | median of run medians | mean p90 | mean GPU busy |')
    print('|---|---|---|---|---|')
    base = None
    for arm in sorted(per_arm):
        runs = per_arm[arm]
        med = st.median(r[0] for r in runs)
        base = med if base is None else base
        print(f'| {arm} | {len(runs)} | {med:.2f} ({(med - base) / base * 100:+.1f}%) | '
              f'{st.mean(r[1] for r in runs):.2f} | {st.mean(r[2] for r in runs):.2f} |')
