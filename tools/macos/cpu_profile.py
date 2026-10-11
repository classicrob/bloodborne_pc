#!/usr/bin/env python3
"""Read BB_CPU_PROFILE v1 JSONL without attaching to the captured process."""
import argparse
from collections import Counter, defaultdict
import json
from pathlib import Path
import re
import subprocess
import sys


def read_capture(path):
    rows = []
    lines = Path(path).read_text().splitlines()
    for i, line in enumerate(lines):
        try:
            rows.append(json.loads(line))
        except json.JSONDecodeError:
            if i != len(lines) - 1:
                raise
            print('Warning: incomplete final record ignored (abrupt process exit).', file=sys.stderr)
    config = next(r for r in rows if r['type'] == 'config')
    if config['version'] != 1:
        raise ValueError('unsupported profile version')
    return rows, config


class Symbols:
    def __init__(self, rows, link_json=None, symbolize=True):
        self.images = [dict(r) for r in rows if r['type'] == 'image']
        self.guest = [r for r in rows if r['type'] == 'guest']
        self.guest.sort(key=lambda r: (r['module'] == -1, r['size']))
        self.modules = {}
        if link_json:
            link = json.loads(Path(link_json).read_text())
            for i, module in enumerate(link['modules']):
                self.modules[i] = Path(module.get('file', module.get('name', f'prx[{i}]'))).name
        self.cache = {}
        self.symbolize = symbolize
        self.warnings = set()
        for removed in (r for r in rows if r['type'] == 'image-remove'):
            candidates = [im for im in self.images if im['base'] == removed['base'] and im['ticks'] <= removed['ticks']]
            if candidates:
                max(candidates, key=lambda im: im['ticks'])['removed'] = removed['ticks']

    def owner(self, pc, ticks):
        for r in self.guest:
            if r['base'] <= pc < r['base'] + r['size']:
                name = 'eboot' if r['module'] == -1 else self.modules.get(r['module'], f"prx[{r['module']}]")
                return ('guest', name, r['base'], None)
        for im in reversed(self.images):
            if im['base'] <= pc < im['end'] and im['ticks'] <= ticks < im.get('removed', float('inf')):
                return ('host', Path(im['path']).name, im['base'], im)
        return ('other', 'unknown', 0, None)

    def prepare(self, samples):
        groups = defaultdict(set)
        for s in samples:
            for i, value in enumerate(s['pc']):
                pc = value - (1 if i and value else 0)
                kind, name, base, im = self.owner(pc, s['ticks'])
                if kind == 'host':
                    key = (im['path'], base, im['uuid'])
                    groups[key].add(pc)
        if not self.symbolize:
            return
        for (path, base, uuid), pcs in groups.items():
            if not Path(path).is_file():
                self.warnings.add(f'image unavailable: {path} (offsets only)')
                continue
            try:
                ids = subprocess.run(['dwarfdump', '--uuid', path], check=True, capture_output=True, text=True).stdout
                uuids = {m.replace('-', '').lower() for m in re.findall(r'UUID: ([0-9A-Fa-f-]+)', ids)}
                if uuid.lower() not in uuids:
                    self.warnings.add(f'image UUID mismatch: {path} (offsets only)')
                    continue
                ordered = sorted(pcs)
                for start in range(0, len(ordered), 256):
                    batch = ordered[start:start + 256]
                    output = subprocess.run(['atos', '-arch', 'x86_64', '-o', path, '-l', hex(base),
                                             *map(hex, batch)], check=True, capture_output=True, text=True).stdout.splitlines()
                    if len(output) != len(batch):
                        raise ValueError('atos returned an unexpected record count')
                    for pc, symbol in zip(batch, output):
                        if not re.fullmatch(r'0x[0-9a-fA-F]+', symbol.strip()):
                            self.cache[(path, base, uuid, pc)] = symbol.split(" (in ", 1)[0]
            except (OSError, ValueError, subprocess.CalledProcessError) as error:
                self.warnings.add(f'symbolization unavailable: {path}: {error} (offsets only)')

    def describe(self, pc, ticks):
        kind, name, base, im = self.owner(pc, ticks)
        label = f'{name}+0x{pc-base:x}'
        if kind == 'guest':
            return label, 'guest'
        if kind != 'host':
            return label, 'other'
        symbol = self.cache.get((im['path'], base, im['uuid'], pc))
        if symbol:
            label = f'{name}!{symbol}'
        if 'MoltenVK' in name or (symbol and re.search(r'\b(?:MVK|mvk)[A-Za-z_]', symbol)):
            category = 'MoltenVK'
        elif 'libbbgpu' in name:
            category = 'GPU-library'
        elif name == 'bb-probe':
            category = 'HLE-runtime'
        else:
            category = 'other'
        return label, category


def report(rows, config, symbols, top, folded):
    samples = [r for r in rows if r['type'] == 'sample']
    symbols.prepare(samples)
    flat = defaultdict(Counter)
    categories = defaultdict(Counter)
    incomplete = Counter()
    stacks = Counter()
    for sample in samples:
        thread = f"{sample['name'] or '<uncached>'} pthread=0x{sample['pthread']:x}"
        labels = [symbols.describe(pc - (1 if i and pc else 0), sample['ticks'])
                  for i, pc in enumerate(sample['pc'])]
        if not labels:
            continue
        flat[thread][labels[0][0]] += 1
        categories[thread][labels[0][1]] += 1
        if sample['flags']:
            incomplete[thread] += 1
        stack = [thread, *(label.replace(';', ':').replace('\n', ' ') for label, _ in reversed(labels))]
        stacks[';'.join(stack)] += 1
    print(f"Requested {config['hz']} Hz process CPU timer; captured {len(samples)} samples. Counts are not exact CPU milliseconds.")
    summaries = [r for r in rows if r['type'] == 'summary']
    print('Loss/finish:', json.dumps(summaries[-1], sort_keys=True) if summaries else 'no final summary; abrupt/ongoing capture')
    for thread, counts in sorted(flat.items(), key=lambda item: -item[1].total()):
        total = counts.total()
        print(f'\n{thread}: {total} samples; {incomplete[thread]} uncached/failed-read samples')
        print('  Split:', ', '.join(f'{name} {n/total:.1%}' for name, n in categories[thread].most_common()))
        for name, n in counts.most_common(top):
            print(f'  {n:7d} {n/total:6.1%} {name}')
    if folded:
        Path(folded).write_text(''.join(f'{stack} {n}\n' for stack, n in sorted(stacks.items())))
    for warning in sorted(symbols.warnings):
        print('Warning:', warning, file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture')
    parser.add_argument('--link-json', help='link_modules.py metadata for PRX names')
    parser.add_argument('--top', type=int, default=20)
    parser.add_argument('--folded', help='write counted stacks for flamegraph tools')
    parser.add_argument('--no-symbols', action='store_true', help='image/guest offsets only; no atos subprocess')
    args = parser.parse_args()
    rows, config = read_capture(args.capture)
    report(rows, config, Symbols(rows, args.link_json, not args.no_symbols), args.top, args.folded)


if __name__ == '__main__':
    main()
