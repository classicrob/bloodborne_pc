#!/usr/bin/env python3
"""Summarize a BB_PERF JSONL file (docs/macos/perf.md): one line per 5 s window, then totals.

  perf_summary.py perf.jsonl
Fails when the file has no schema record or no window."""
import json
import sys


def p(metrics, name, key):
    value = metrics.get(name, {}).get(key)
    return '-' if value is None else f'{value:.2f}'


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    records = [json.loads(line) for line in open(sys.argv[1]) if line.strip()]
    schema = [r for r in records if r.get('type') == 'schema']
    windows = [r for r in records if r.get('type') == 'window']
    if len(schema) != 1 or schema[0].get('version') != 2:
        sys.exit('perf_summary: expected exactly one version-2 schema record')
    if not windows:
        sys.exit('perf_summary: no window records (run shorter than 5 s, or killed before the first)')
    perturbed = schema[0]['perturbed']
    print(f"BB_PERF {'PERTURBED (BB_PERF_PHASES=1): attribution only, not FPS' if perturbed else 'unperturbed'}")
    print(f"{'t(s)':>7} {'FPS':>6} {'GPU/flip':>9} {'flip p50':>9} {'flip p95':>9} {'flip max':>9} "
          f"{'LvpCPU p50':>10} {'p95':>6} {'fence p95':>9} {'subm':>5}" +
          (f" {'scene':>7} {'post':>7} {'upsc':>7} {'frame p95':>9}" if perturbed else ''))
    flips = seconds = busy = 0.0
    for w in windows:
        m, g = w['metrics'], w['gpu']
        fps = w['flips'] / w['seconds'] if w['seconds'] else 0
        flips += w['flips']; seconds += w['seconds']; busy += g['busy_union_ms']
        per_flip = '-' if g['busy_union_ms_per_flip'] is None else f"{g['busy_union_ms_per_flip']:.2f}"
        line = (f"{w['elapsed_seconds']:7.1f} {fps:6.1f} {per_flip:>9} {p(m, 'guest_flip_interval_ms', 'p50'):>9} "
                f"{p(m, 'guest_flip_interval_ms', 'p95'):>9} {p(m, 'guest_flip_interval_ms', 'max'):>9} "
                f"{p(m, 'liverpool_cpu_ms', 'p50'):>10} {p(m, 'liverpool_cpu_ms', 'p95'):>6} "
                f"{p(m, 'fence_wait_wall_ms', 'p95'):>9} {g['main_submissions']:>5}")
        if perturbed:
            line += (f" {p(m, 'gpu_scene_ms', 'mean'):>7} {p(m, 'gpu_post_ms', 'mean'):>7} "
                     f"{p(m, 'gpu_upscaler_ms', 'mean'):>7} {p(m, 'gpu_frame_work_ms', 'p95'):>9}")
        print(line + (' final' if w.get('final') else ''))
    print(f"total: {seconds:.1f} s, {flips:.0f} flips, {flips / seconds if seconds else 0:.2f} FPS, "
          f"GPU busy {busy / flips if flips else 0:.2f} ms/flip (union of submissions, both streams)")


if __name__ == '__main__':
    main()
