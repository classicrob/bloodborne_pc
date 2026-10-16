#!/usr/bin/env python3
"""Summarize replay logs and preserve detached qualification verdicts.
Submit-to-GPU-done wall distributions cover iterations 2..N. GPU timestamp
collection windows are diagnostics without exact iteration membership, never
accepted per-iteration busy times. No replay result establishes live FPS.
"""
import argparse
import json
from pathlib import Path
import re
import statistics as st
import math

VERDICT_PREFIX = 'REPLAY_QUALIFICATION_JSON: '


def qualify(result, context=None):
    context = context or {}
    if not isinstance(context, dict):
        raise ValueError('qualification context must be an object')
    gates = context.get('source_gates', {})
    if not isinstance(gates, dict):
        raise ValueError('qualification source gates must be an object')
    source_pass = (set(gates) == {'A', 'B'} and all(v == 'PASS' for v in gates.values()) and
                   context.get('qualification_only') is False)
    comparison = context.get('comparison_status', 'NOT_EVALUATED')
    reference = context.get('reference_workload')
    if comparison == 'MATCHED' and (not reference or result['workload_signatures'] != reference):
        comparison = 'INCOMPARABLE'
    if not result['workload_consistent']:
        comparison = 'INCOMPARABLE'
    labels = []
    if not source_pass:
        labels.append('SOURCE GATE UNQUALIFIED')
    if context.get('diagnostic') or context.get('diagnostic_reasons'):
        labels.append('DIAGNOSTIC PERTURBED')
    if result['wall_metric'] != 'submit_to_gpu_done_wall_ms':
        labels.append('GPU-DONE WALL UNQUALIFIED')
    if comparison == 'INCOMPARABLE':
        labels.append('INCOMPARABLE')
    elif comparison != 'MATCHED':
        labels.append('COMPARISON NOT EVALUATED')
    qualified = source_pass and not labels and comparison == 'MATCHED'
    result.update(qualification='; '.join(labels) if labels else 'QUALIFIED RELATIVE ATTRIBUTION',
                  qualification_labels=labels, timing_qualified=qualified,
                  comparison_status=comparison, qualification_context=context,
                  accepted_metrics=['submit_to_gpu_done_wall_ms'] if qualified else [],
                  limitation='repeated workload; pixels/live FPS and per-iteration GPU busy unqualified')
    return result


def compare_work(results):
    signatures = {tuple(signature) for result in results for signature in result['workload_signatures']}
    return ('MATCHED' if len(results) >= 2 and len(signatures) == 1 and
            all(r['workload_consistent'] for r in results) else 'INCOMPARABLE')


def verdict_text(result):
    return result['qualification'] + ' | ' + result['limitation']

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
    rows, collection, fields, block, work = {}, {}, {}, [], {}
    contexts = []
    for line in text.splitlines():
        if line.startswith(VERDICT_PREFIX):
            contexts.append(json.loads(line[len(VERDICT_PREFIX):]))
        if line.startswith('Parse split'):
            block = [line]; continue
        if block and line.startswith(' '):
            block.append(line)
        m = re.match(r'Perf replay iteration (\d+)[^:]*: ([\d.]+) ms submit-to-(idle|GPU-done).*?GPU thread CPU ([\d.]+)', line)
        if m:
            iteration = int(m[1])
            if iteration in rows:
                raise ValueError(f'{path}: duplicate iteration {iteration}')
            done = re.search(r'([\d.]+) ms submit-to-GPU-done', line)
            rows[iteration] = (float(done[1]) if done else None, float(m[4]), float(m[2]))
            counts = re.search(r'; (\d+) draws, (\d+) dispatches, (\d+) submissions; images registered (\d+), image uploads ([\d.]+) MiB, buffer uploads ([\d.]+) MiB', line)
            if counts:
                work[iteration] = [int(counts[i]) for i in range(1, 5)] + [float(counts[i]) for i in (5, 6)]
            fields[iteration] = {}
            for key, pattern in FIELDS:
                match = re.search(pattern, '\n'.join(block))
                if match:
                    fields[iteration][key] = float(match[1])
            block = []
        m = re.match(r'  GPU (timestamp collection window at iteration|busy of iteration) (\d+)[^:]*: ([\d.]+) ms', line)
        if m:
            iteration = int(m[2])
            if iteration in collection:
                raise ValueError(f'{path}: duplicate GPU collection-window label {iteration}')
            submissions = re.search(r'\((\d+) submissions\)', line)
            collection[iteration] = {'label_iteration': iteration, 'ms': float(m[3]),
                                    'legacy_approximate_label': m[1] == 'busy of iteration',
                                    'submissions': int(submissions[1]) if submissions else None}
    if len(contexts) > 1:
        raise ValueError(f'{path}: duplicate qualification context')
    if not rows or max(rows) < 2:
        raise ValueError(f'{path}: no steady-state iterations')
    count = expected if expected is not None else max(rows)
    if sorted(rows) != list(range(1, count + 1)):
        raise ValueError(f'{path}: missing/out-of-range iterations; expected 1..{count}, got {sorted(rows)}')
    if expected is not None and (not re.search(rf'Perf replay steady state \(iterations 2\.\.{count}\)', text) or
                                 'capture -> restore -> capture again is exact' not in text):
        raise ValueError(f'{path}: missing complete run/image round-trip evidence')
    if expected is not None and sorted(work) != list(range(1, count + 1)):
        raise ValueError(f'{path}: missing steady draw/dispatch/guest-submission counts')
    if any(not math.isfinite(row['ms']) or row['ms'] < 0 for row in collection.values()):
        raise ValueError(f'{path}: invalid GPU collection-window diagnostic')
    if require_busy and not any(row['ms'] > 0 for row in collection.values()):
        raise ValueError(f'{path}: missing positive GPU collection-window diagnostic (--require-busy is diagnostic-only)')
    done_complete = all(rows[i][0] is not None for i in range(2, count + 1))
    if expected is not None and not done_complete:
        raise ValueError(f'{path}: missing submit-to-GPU-done wall sample; idle time is not a substitute')
    wall = [rows[i][0 if done_complete else 2] for i in range(2, count + 1)]
    if any(not math.isfinite(value) or value <= 0 for value in wall):
        raise ValueError(f'{path}: invalid wall time')
    gpu = [row['ms'] for row in collection.values()]
    signatures = [list(value) for value in sorted({tuple(work[i][:3]) for i in range(2, count + 1) if i in work})]
    result = {'path': str(path), 'samples': len(wall), 'median_ms': st.median(wall),
              'p90_ms': percentile(wall, .9), 'p95_ms': percentile(wall, .95), 'p99_ms': percentile(wall, .99),
              'max_ms': max(wall), 'gpu_thread_cpu_median_ms': st.median(rows[i][1] for i in range(2, count + 1)),
              'wall_metric': 'submit_to_gpu_done_wall_ms' if done_complete else 'submit_to_idle_wall_ms',
              'gpu_busy_samples': 0, 'gpu_busy_median_ms': None,
              'gpu_collection_window_samples': list(collection.values()),
              'gpu_collection_window_median_ms': st.median(gpu) if gpu else None,
              'gpu_collection_window_qualification': 'diagnostic only; deferred reads have no exact iteration membership',
              'fields': {key: st.median(fields[i][key] for i in range(2, count + 1) if key in fields[i])
                         for key, _ in FIELDS if any(key in fields[i] for i in range(2, count + 1))},
              'work_per_iteration': work, 'workload_signatures': signatures,
              'workload_consistent': len(signatures) == 1 and len(work) == count}
    return qualify(result, contexts[0] if contexts else None)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--arms', action='store_true')
    parser.add_argument('--expected', type=int)
    parser.add_argument('--require-busy', action='store_true',
                        help='legacy name: require a positive collection-window diagnostic; never qualifies per-iteration busy')
    parser.add_argument('--json', action='store_true')
    parser.add_argument('logs', nargs='+', type=Path)
    args = parser.parse_args()
    try:
        results = [summarize(path, args.expected, args.require_busy) for path in args.logs]
    except ValueError as error:
        parser.exit(2, f'STOP: replay summary: {error}\n')
    if len(results) > 1 and compare_work(results) != 'MATCHED':
        for result in results:
            qualify(result, dict(result['qualification_context'], comparison_status='INCOMPARABLE'))
    incomparable = any(r['comparison_status'] == 'INCOMPARABLE' for r in results)
    if args.json:
        print(json.dumps(results, indent=2))
        if incomparable:
            parser.exit(2, 'INCOMPARABLE: draw/dispatch/guest-submission counts differ; results retained\n')
        return
    arms = {}
    for result in results:
        name = Path(result['path']).name
        gpu = result['gpu_collection_window_median_ms']
        print(f"{name}: {verdict_text(result)} | n={result['samples']} {result['wall_metric']} median {result['median_ms']:.2f} p90 {result['p90_ms']:.2f} "
              f"p95 {result['p95_ms']:.2f} p99 {result['p99_ms']:.2f} max {result['max_ms']:.2f} | "
              f"GPU collection-window DIAGNOSTIC {f'{gpu:.2f}' if gpu is not None else 'MISSING'} "
              f"(n={len(result['gpu_collection_window_samples'])}; not per-iteration busy)")
        print('   ' + '; '.join(f'{key} {value:.2f}' for key, value in result['fields'].items()))
        arm = Path(result['path']).stem.rsplit('-', 1)[-1]
        arms.setdefault(arm, []).append(result['median_ms'])
    if args.arms and len(arms) > 1:
        print('Comparison verdicts: ' + ' | '.join(sorted({r['qualification'] for r in results})))
        print('\n| arm | runs | median of run medians ms |\n|---|---|---|')
        for arm, medians in sorted(arms.items()):
            print(f'| {arm} | {len(medians)} | {st.median(medians):.2f} |')
    if incomparable:
        parser.exit(2, 'INCOMPARABLE: draw/dispatch/guest-submission counts differ; results retained\n')


if __name__ == '__main__':
    main()
