#!/usr/bin/env python3
"""Rank observed HLE call wall time; never reinterpret it as CPU cost."""
import argparse
import collections
import json
import re

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('log')
parser.add_argument('--windows', type=int, default=3)
parser.add_argument('--min-window-ms', type=float, default=10000)
parser.add_argument('--call-loop-ns', '--boundary-ns', dest='call_loop_ns', type=float, default=0,
                    help='assumed warm complete PLT call-loop CPU ns; not an isolated boundary')
parser.add_argument('--instrument-ns', type=float, default=357)
args = parser.parse_args()
if args.windows <= 0 or args.call_loop_ns < 0 or args.instrument_ns < 0:
    parser.error('windows must be positive; costs nonnegative')
header = re.compile(r'HLE-SUMMARY window_ms=([\d.]+) flips=(\d+)')
row = re.compile(r'HLE-SUMMARY pthread=(\S+) name="([^"]*)" import=(\S+) completed=(\d+) call_wall_ms=([\d.]+) max_ms=([\d.]+)')
windows = []
with open(args.log) as stream:
    for line in stream:
        h = header.search(line)
        if h:
            windows.append({'ms': float(h[1]), 'flips': int(h[2]), 'rows': []})
        elif windows:
            r = row.search(line)
            if r:
                windows[-1]['rows'].append((r[1], r[2], r[3], int(r[4]), float(r[5]), float(r[6])))
windows = [w for w in windows if w['flips'] and w['ms'] >= args.min_window_ms][-args.windows:]
if not windows:
    parser.error('no qualifying windows')
flips = sum(w['flips'] for w in windows)
wall_ms = sum(w['ms'] for w in windows)
totals = collections.defaultdict(lambda: [0, 0.0, 0.0])
all_count = 0
for w in windows:
    for ident, thread, symbol, count, ms, maximum in w['rows']:
        all_count += count
        if thread not in ('main', 'GXRenderThread'):
            continue
        key = (thread, ident, symbol)
        totals[key][0] += count
        totals[key][1] += ms
        totals[key][2] = max(totals[key][2], maximum)
result = []
for (thread, ident, symbol), (count, ms, maximum) in totals.items():
    result.append(dict(thread=thread, pthread=ident, symbol=symbol,
        completed=count, calls_per_flip=count/flips,
        inclusive_wall_ms_per_flip=ms/flips, max_call_wall_ms=maximum,
        hypothetical_trivial_call_loop_cpu_ms_per_flip=count*args.call_loop_ns/1e6/flips,
        estimated_instrument_cpu_ms_per_flip=count*args.instrument_ns/1e6/flips))
result.sort(key=lambda r: r['inclusive_wall_ms_per_flip'], reverse=True)
print(json.dumps(dict(log=args.log, windows=windows and len(windows), flips=flips,
    window_ms=wall_ms, all_threads_completed=all_count,
    assumed_call_loop_cpu_ns=args.call_loop_ns,
    assumed_instrument_cpu_ns=args.instrument_ns,
    assumption_source="Caller inputs; 357ns instrumentation default is from a separate earlier benchmark, not this run.",
    note='Observed filtered imports only. Inclusive completion-charged wall durations are not CPU body times. Thread rows overlap and cannot be added as frame time.',
    ranked=result), indent=2))
