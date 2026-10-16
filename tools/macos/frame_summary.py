#!/usr/bin/env python3
"""Exact guest Flip-entry interval statistics from opt-in BB_PERF_FRAMES windows.

Bounds are seconds on the reporter clock; both endpoints must lie within them.
This measures guest flip cadence, not actual display presentation or GPU work.
"""
import argparse
import json
import math


def summarize(records, start=0.0, end=None):
    if not math.isfinite(start) or start < 0 or (end is not None and
            (not math.isfinite(end) or end <= start)):
        raise ValueError('need finite 0 <= start < end')
    iterator = iter(records)
    schema = next(iterator, {})
    if (schema.get('type') != 'schema' or schema.get('version') != 3 or
            schema.get('frame_samples') is not True or schema.get('perturbed') is not False):
        raise ValueError('requires unperturbed version-3 BB_PERF_FRAMES schema first')
    lo, hi = round(start * 1e9), None if end is None else round(end * 1e9)
    selected = []
    previous_end = None
    sequence = total = excluded = windows = 0
    terminal = False
    for record in iterator:
        if record.get('type') == 'queue':
            continue
        if record.get('type') != 'window' or terminal or record.get('perturbed') is not False:
            raise ValueError('unexpected record, perturbed window, or data after final window')
        flips, seq, samples = (record.get(k) for k in ('flips', 'flip_sequence_end', 'flip_samples'))
        if (type(flips) is not int or flips < 0 or type(seq) is not int or
                seq != sequence + flips or not isinstance(samples, list)):
            raise ValueError('missing/invalid frame sequence or raw samples')
        expected = flips - (1 if sequence == 0 and flips else 0)
        if len(samples) != expected or len(samples) > 16384:
            raise ValueError('raw interval count does not match flip sequence')
        for sample in samples:
            if (not isinstance(sample, list) or len(sample) != 2 or
                    any(type(v) is not int or v <= 0 for v in sample)):
                raise ValueError('raw sample must contain positive integer nanoseconds')
            at, duration = sample
            if at < duration or (previous_end is not None and at - previous_end != duration):
                raise ValueError('noncontiguous/backward guest flip intervals')
            previous_end = at
            total += 1
            if at - duration >= lo and (hi is None or at <= hi):
                selected.append(duration)
            else:
                excluded += 1
        sequence, terminal = seq, record.get('final') is True
        windows += 1
    if not selected:
        raise ValueError('no complete intervals within selected bounds')
    selected.sort()
    n = len(selected)
    rank = lambda p: selected[math.ceil(p * n) - 1] / 1e6
    slow = selected[-math.ceil(n * .01):]
    return {
        'source': 'VideoOutDriver::Flip entry (not actual display presentation)',
        'start_seconds': start, 'end_seconds': end,
        'windows': windows, 'flips_recorded': sequence, 'raw_intervals': total,
        'intervals_selected': n, 'intervals_outside_bounds': excluded,
        'first_flip_without_interval': int(sequence > 0), 'invalid_or_dropped': 0,
        'final_window_present': terminal,
        'interval_seconds': sum(selected) / 1e9,
        'fps': n * 1e9 / sum(selected),
        'median_ms': rank(.5), 'p95_ms': rank(.95), 'p99_ms': rank(.99),
        'max_ms': selected[-1] / 1e6,
        'one_percent_low_fps': len(slow) * 1e9 / sum(slow),
        'one_percent_low_definition': 'reciprocal mean of slowest ceil(0.01*N) intervals',
        'over_60hz_budget': sum(v * 60 > 1_000_000_000 for v in selected),
        'over_50ms': sum(v > 50_000_000 for v in selected),
        'over_100ms': sum(v > 100_000_000 for v in selected),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('path')
    parser.add_argument('--start', type=float, default=0.0)
    parser.add_argument('--end', type=float)
    args = parser.parse_args()
    try:
        with open(args.path) as source:
            result = summarize((json.loads(line) for line in source if line.strip()), args.start, args.end)
    except (OSError, ValueError) as e:
        parser.exit(1, f'frame_summary: {e}\n')
    print(json.dumps(result, separators=(',', ':'), allow_nan=False))


if __name__ == '__main__':
    main()
