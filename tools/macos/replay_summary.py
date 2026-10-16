#!/usr/bin/env python3
"""Summarize replay logs. Strict mode rejects STOPs, missing/duplicate iterations and GPU samples.
GPU busy is reported for iterations 2..N-1 (the timestamp readback is one iteration delayed).
Wall/CPU distributions cover iterations 2..N. These are replay workload times, not live FPS.
"""
import argparse
import json
from pathlib import Path
import re
import statistics as st
import math

FIELDS = [('A on CPU', r'stage A \(GPU command thread, [\d.]+%, on CPU ([\d.]+) ms'),
          ('B on CPU', r'stage B \(draw recording thread, [\d.]+%, on CPU ([\d.]+) ms'),
          ('VK on CPU', r'Vulkan recording thread \([\d.]+%, on CPU ([\d.]+) ms'),
          ('B queue submit', r'stage B .*in scopes: .*?queue submit ([\d.]+)'),
          ('VK queue submit', r'Vulkan recording thread .*?queue submit ([\d.]+)'),
          ('B deferred before WRITE_DATA', r'deferred signals before WRITE_DATA ([\d.]+)'),
          ('A drain DUMP_CONST_RAM', r'stage B drain at DUMP_CONST_RAM ([\d.]+)'),
          ('flushes/frame', r'Flushes of the draw scheduler: ([\d.]+) per frame')]


def percentile(values, fraction):
    return sorted(values)[max(0, math.ceil(fraction * len(values)) - 1)]


def summarize(path, expected=None, require_busy=False):
    text = path.read_text(errors='replace')
    if re.search(r'(?m)^(STOP:|WATCHDOG:)', text):
        raise ValueError(f'{path}: STOP/watchdog; no successful timing result')
    rows, busy, fields, block, work = {}, {}, {}, [], {}
    for line in text.splitlines():
        if line.startswith('Parse split'):
            block = [line]; continue
        if block and line.startswith(' '):
            block.append(line)
        m = re.match(r'Perf replay iteration (\d+)[^:]*: ([\d.]+) ms submit-to-idle.*?GPU thread CPU ([\d.]+)', line)
        if m:
            iteration = int(m[1])
            if iteration in rows:
                raise ValueError(f'{path}: duplicate iteration {iteration}')
            rows[iteration] = (float(m[2]), float(m[3]))
            counts = re.search(r'; (\d+) draws, (\d+) dispatches, (\d+) submissions; images registered (\d+), image uploads ([\d.]+) MiB, buffer uploads ([\d.]+) MiB', line)
            if counts:
                work[iteration] = [int(counts[i]) for i in range(1, 5)] + [float(counts[i]) for i in (5, 6)]
            fields[iteration] = {}
            for key, pattern in FIELDS:
                match = re.search(pattern, '\n'.join(block))
                if match:
                    fields[iteration][key] = float(match[1])
            block = []
        m = re.match(r'  GPU busy of iteration (\d+).*: ([\d.]+) ms', line)
        if m:
            iteration = int(m[1])
            if iteration in busy:
                raise ValueError(f'{path}: duplicate GPU busy sample {iteration}')
            busy[iteration] = float(m[2])
    if not rows or max(rows) < 2:
        raise ValueError(f'{path}: no steady-state iterations')
    count = expected if expected is not None else max(rows)
    if sorted(rows) != list(range(1, count + 1)):
        raise ValueError(f'{path}: missing/out-of-range iterations; expected 1..{count}, got {sorted(rows)}')
    if expected is not None and (not re.search(rf'Perf replay steady state \(iterations 2\.\.{count}\)', text) or
                                 'capture -> restore -> capture again is exact' not in text):
        raise ValueError(f'{path}: missing complete run/image round-trip evidence')
    if expected is not None and (sorted(work) != list(range(1, count + 1)) or
                                 len({tuple(work[i][:3]) for i in range(2, count + 1)}) != 1):
        raise ValueError(f'{path}: missing or non-equivalent steady draw/dispatch/submission counts')
    gpu_iterations = list(range(2, count))
    if require_busy and (not gpu_iterations or any(i not in busy or busy[i] <= 0 for i in gpu_iterations)):
        raise ValueError(f'{path}: missing/nonpositive GPU busy samples for iterations 2..{count-1}')
    wall = [rows[i][0] for i in range(2, count + 1)]
    if any(not math.isfinite(value) or value <= 0 for value in wall):
        raise ValueError(f'{path}: invalid wall time')
    gpu = [busy[i] for i in gpu_iterations if i in busy]
    result = {'path': str(path), 'samples': len(wall), 'median_ms': st.median(wall),
              'p90_ms': percentile(wall, .9), 'p95_ms': percentile(wall, .95), 'p99_ms': percentile(wall, .99),
              'max_ms': max(wall), 'gpu_thread_cpu_median_ms': st.median(rows[i][1] for i in range(2, count + 1)),
              'gpu_busy_samples': len(gpu), 'gpu_busy_median_ms': st.median(gpu) if gpu else None,
              'fields': {key: st.median(fields[i][key] for i in range(2, count + 1) if key in fields[i])
                         for key, _ in FIELDS if any(key in fields[i] for i in range(2, count + 1))},
              'work_per_iteration': work, 'qualification': 'repeated workload, pixels/live FPS unqualified'}
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--arms', action='store_true')
    parser.add_argument('--expected', type=int)
    parser.add_argument('--require-busy', action='store_true')
    parser.add_argument('--json', action='store_true')
    parser.add_argument('logs', nargs='+', type=Path)
    args = parser.parse_args()
    try:
        results = [summarize(path, args.expected, args.require_busy) for path in args.logs]
    except ValueError as error:
        parser.exit(2, f'STOP: replay summary: {error}\n')
    if args.json:
        print(json.dumps(results, indent=2)); return
    arms = {}
    for result in results:
        name = Path(result['path']).name
        gpu = result['gpu_busy_median_ms']
        print(f"{name}: n={result['samples']} wall median {result['median_ms']:.2f} p90 {result['p90_ms']:.2f} "
              f"p95 {result['p95_ms']:.2f} p99 {result['p99_ms']:.2f} max {result['max_ms']:.2f} | "
              f"GPU busy {f'{gpu:.2f}' if gpu is not None else 'MISSING'} (n={result['gpu_busy_samples']})")
        print('   ' + '; '.join(f'{key} {value:.2f}' for key, value in result['fields'].items()))
        arm = Path(result['path']).stem.rsplit('-', 1)[-1]
        arms.setdefault(arm, []).append(result['median_ms'])
    if args.arms and len(arms) > 1:
        print('\n| arm | runs | median of run medians ms |\n|---|---|---|')
        for arm, medians in sorted(arms.items()):
            print(f'| {arm} | {len(medians)} | {st.median(medians):.2f} |')


if __name__ == '__main__':
    main()
