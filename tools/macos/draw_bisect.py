#!/usr/bin/env python3
"""Unattended draw bisection for a stable bad frame (BB_DRAW_BISECT, diagnostic builds only).

  draw_bisect.py --run RUN --roi X0,Y0,X1,Y1 [--label NAME] [--captures 3]
  draw_bisect.py --selftest

RUN is a play_route run started with --diag-env BB_DRAW_BISECT=<file> and standing still at the
pose that shows the defect. ROI is the defect's region as fractions of the window (e.g. the
floating shelf). The game's draws are numbered per frame; the script skips index ranges and asks
whether the ROI changes against the baseline (window_tool roidiff, thresholds from the noise
between baseline captures):

  search(lo, hi): skip lo..mid; changed -> search(lo, mid)
                  otherwise skip mid+1..hi; changed -> search(mid+1, hi)
                  neither half alone changes it -> ambiguous (several draws, or none): stop, review
When several draws make the defect, it follows the first half that changes the ROI and so names
one of them; skipping that one draw is confirmed, the others are not searched.

A step whose captures disagree, or fall between the thresholds, is retaken once; if still unclear
it goes to review.md for a human look instead of a guess. At the end the found draw is listed,
confirmed with "skip vs <hash>" and dumped with "dump vs <hash>". Everything lands in
RUN/bisect-NAME/ (report.json, review.md, steps.log, captures).
"""
import argparse, json, os, re, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / 'out/macos_route/bin/window_tool'
CONFIRM = re.compile(r'^Draw bisect: frame (\d+): (\d+) draws, (\d+) skipped')
REJECT = re.compile(r'^Draw bisect: .*(reject|could not be read)')
LISTED = re.compile(r'^Draw bisect: frame (\d+) listed: (\d+) draws')
FPS = re.compile(r'^Frame stats: ([\d.]+) FPS')


def roidiff(a, b, roi):
    out = subprocess.run([str(TOOL), 'roidiff', str(a), str(b), *map(str, roi)], capture_output=True, text=True)
    if out.returncode:
        raise SystemExit(f'draw_bisect: roidiff failed: {out.stderr.strip()}')
    return json.loads(out.stdout)['mean_abs']


class Game:
    """The running game: control file, game.log and play_route captures."""

    def __init__(self, run, captures):
        self.run, self.captures = Path(run), captures
        env = json.loads((self.run / 'manifest.json').read_text())['env']
        if 'BB_DRAW_BISECT' not in env:
            raise SystemExit('draw_bisect: this run was not started with --diag-env BB_DRAW_BISECT=<file>')
        self.control = Path(env['BB_DRAW_BISECT'])
        self.log = self.run / 'game.log'

    def alive(self):
        return not (self.run / 'exit.json').exists()

    def lines_from(self, offset):
        with self.log.open('rb') as f:
            f.seek(offset)
            return f.read().decode(errors='replace').splitlines()

    def write(self, rules):
        """Writes the rules (renamed into place) and waits for the game to apply them."""
        offset = self.log.stat().st_size
        tmp = self.control.with_suffix('.tmp')
        tmp.write_text('\n'.join(rules) + '\n')
        os.replace(tmp, self.control)
        deadline = time.time() + 120
        while time.time() < deadline:
            if not self.alive():
                raise SystemExit('draw_bisect: the game exited')
            for line in self.lines_from(offset):
                if REJECT.search(line):
                    raise SystemExit(f'draw_bisect: the game rejected the rules {rules}: {line}')
                m = CONFIRM.search(line)
                if m:
                    return {'frame': int(m.group(1)), 'draws': int(m.group(2)), 'skipped': int(m.group(3)),
                            'offset': offset}
            time.sleep(0.5)
        raise SystemExit(f'draw_bisect: no "Draw bisect: frame F: N draws" line within 120 s for {rules}')

    def settle(self):
        """Two more frames at the current frame rate, plus a second for the presenter."""
        fps = [float(m.group(1)) for m in map(FPS.search, self.lines_from(max(0, self.log.stat().st_size - 200000))) if m]
        time.sleep(1 + 2 / max(0.2, fps[-1] if fps else 1.0))

    def capture(self, stage):
        shots = []
        for k in range(self.captures):
            out = subprocess.run([sys.executable, str(ROOT / 'tools/macos/play_route.py'), '--run', str(self.run),
                                  'capture', f'{stage}-{k + 1}'], capture_output=True, text=True)
            record = json.loads(out.stdout.strip().splitlines()[-1]) if out.returncode == 0 and out.stdout.strip() else None
            if not record or 'path' not in record:
                raise SystemExit(f'draw_bisect: capture failed: {out.stderr.strip()[-300:]}')
            shots.append(record['path'])
            time.sleep(1.5)
        return shots

    def listing(self, offset, frame_hint=None):
        """The draw lines of the next listed frame after offset: {index: line}."""
        deadline = time.time() + 120
        while time.time() < deadline:
            lines = self.lines_from(offset)
            listed = [LISTED.search(l) for l in lines]
            done = [m for m in listed if m]
            if done:
                frame = done[0].group(1)
                rows = {}
                for l in lines:
                    m = re.match(rf'^Draw bisect: frame {frame} draw (\d+)(?: SKIPPED)?: (.*)', l)
                    if m:
                        rows[int(m.group(1))] = m.group(2)
                return int(frame), rows
            time.sleep(0.5)
        raise SystemExit('draw_bisect: no listing within 120 s')

    def dump_lines(self, offset, vs):
        deadline = time.time() + 60
        while time.time() < deadline:
            rows = [l for l in self.lines_from(offset) if f'dump vs {vs}' in l]
            if rows:
                time.sleep(2)
                return [l for l in self.lines_from(offset) if f'dump vs {vs}' in l]
            time.sleep(0.5)
        return []


class Bisect:
    def __init__(self, game, roi, out):
        self.game, self.roi, self.out = game, roi, Path(out)
        self.out.mkdir(parents=True, exist_ok=True)
        self.steps, self.review, self.list_value = [], [], 0

    def note(self, text):
        line = f'{time.strftime("%H:%M:%S")} {text}'
        print(f'draw_bisect: {text}', file=sys.stderr, flush=True)
        with (self.out / 'steps.log').open('a') as f:
            f.write(line + '\n')

    def baseline(self):
        self.game.write(['# baseline: nothing skipped'])
        self.game.settle()
        self.base = self.game.capture('bisect-base')
        noise = [roidiff(self.base[0], s, self.roi) for s in self.base[1:]]
        self.noise = max(noise) if noise else 0.5
        self.t_lo = max(1.5, 3 * self.noise)
        self.t_hi = max(4.0, 6 * self.noise)
        self.note(f'baseline noise {self.noise:.2f}; unchanged < {self.t_lo:.2f} <= ambiguous < {self.t_hi:.2f} <= changed')

    def verdict(self, rules, name):
        """True (ROI changed), False (unchanged) or None (unclear after one retake)."""
        info = self.game.write(rules)
        self.game.settle()
        for attempt in (1, 2):
            shots = self.game.capture(f'bisect-{name}-{attempt}')
            values = sorted(roidiff(self.base[0], s, self.roi) for s in shots)
            changed = [v >= self.t_hi for v in values]
            unchanged = [v < self.t_lo for v in values]
            step = {'rules': rules, 'name': name, 'attempt': attempt, 'confirm': info, 'roidiff': values, 'shots': shots}
            self.steps.append(step)
            if all(changed):
                self.note(f'{name} {rules}: CHANGED {values}')
                return True
            if all(unchanged):
                self.note(f'{name} {rules}: unchanged {values}')
                return False
            self.note(f'{name} {rules}: unclear {values} (attempt {attempt})')
        self.review.append(step)
        return None

    def search(self, lo, hi, depth=0):
        if lo == hi:
            return [lo]
        mid = (lo + hi) // 2
        a = self.verdict([f'skip {lo}-{mid}'], f'd{depth}-{lo}-{mid}')
        if a:
            return self.search(lo, mid, depth + 1)
        b = self.verdict([f'skip {mid + 1}-{hi}'], f'd{depth}-{mid + 1}-{hi}')
        if b:
            if a is None:
                self.note(f'lower half {lo}-{mid} was unclear; following the upper half')
            return self.search(mid + 1, hi, depth + 1)
        if a is None or b is None:
            self.note(f'stopping at {lo}-{hi}: a half is unclear (see review.md)')
        else:
            self.note(f'stopping at {lo}-{hi}: neither half alone changes the ROI (several draws?)')
        self.review.append({'range': [lo, hi], 'reason': 'neither half alone changes the ROI or a half is unclear'})
        return list(range(lo, hi + 1)) if hi - lo < 8 else []

    def describe(self, found):
        self.list_value += 1
        offset = self.game.log.stat().st_size
        self.game.write([f'list {self.list_value}'])
        frame, rows = self.game.listing(offset)
        result = []
        for index in found:
            line = rows.get(index, '')
            vs = re.search(r'vs (0x[0-9a-f]+)', line)
            result.append({'index': index, 'listing': line, 'vs': vs.group(1) if vs else None})
            self.note(f'draw {index} (frame {frame}): {line[:220]}')
        for draw in result:
            if not draw['vs']:
                continue
            draw['skip_vs_changed'] = self.verdict([f'skip vs {draw["vs"]}'], f'confirm-vs-{draw["vs"]}')
            offset = self.game.log.stat().st_size
            self.game.write([f'dump vs {draw["vs"]}'])
            draw['dump'] = self.game.dump_lines(offset, draw['vs'])
            self.note(f'dump vs {draw["vs"]}: {len(draw["dump"])} lines')
        return result

    def run(self, total=None):
        offset = self.game.log.stat().st_size
        self.list_value += 1
        self.game.write([f'list {self.list_value}'])
        frame, rows = self.game.listing(offset)
        n = total or len(rows)
        self.note(f'frame {frame}: {n} draws')
        self.baseline()
        all_changed = self.verdict([f'skip 0-{n - 1}'], 'all')
        if all_changed is None:
            self.note('skipping every draw gives an unclear ROI change (captures disagree): see review.md')
            found = []
        elif not all_changed:
            self.note('skipping every draw does not change the ROI: wrong ROI or not a draw defect')
            found = []
        else:
            found = self.search(0, n - 1)
        draws = self.describe(found) if found and len(found) <= 8 else []
        self.game.write(['# done: nothing skipped'])
        report = {'roi': self.roi, 'noise': self.noise, 'thresholds': [self.t_lo, self.t_hi], 'draws_per_frame': n,
                  'found': found, 'draws': draws, 'steps': self.steps, 'review': self.review}
        (self.out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
        md = ['# Draw bisection review', '', f'ROI {self.roi}; baseline {self.base}', '']
        for r in self.review:
            md.append(f"- {r.get('name', r.get('range'))}: {r.get('rules', '')} roidiff {r.get('roidiff', '')}")
            for s in r.get('shots', []):
                md.append(f'  - ![]({s})')
        (self.out / 'review.md').write_text('\n'.join(md) + '\n')
        self.note(f'found {found}; report {self.out / "report.json"}')
        return report


class Fake:
    """Offline stand-in for selftest: real screenshots, simulated skips. The ROI 'changes' when a
    bad draw is skipped (the shelf vanishes); noise is the real capture-to-capture noise."""

    def __init__(self, bad, n, base_shots, changed_shot, unclear=()):
        self.bad, self.n, self.base_shots, self.changed_shot, self.unclear = set(bad), n, base_shots, changed_shot, set(unclear)
        self.rules, self.list_value, self.log = [], 0, Path('/dev/null')

    def write(self, rules):
        self.rules = rules
        return {'frame': 1, 'draws': self.n, 'skipped': 0}

    def settle(self):
        pass

    def skipped(self):
        out = set()
        for r in self.rules:
            m = re.match(r'skip (\d+)-(\d+)$', r)
            if m:
                out |= set(range(int(m.group(1)), int(m.group(2)) + 1))
            m = re.match(r'skip vs (0x[0-9a-f]+)$', r)
            if m:
                out |= {i for i in self.bad if f'0x{i:08x}' == m.group(1)}
        return out

    def capture(self, stage):
        hit = self.skipped() & self.bad
        if hit & self.unclear:  # flaky: the captures disagree, as with an intermittent defect
            return [self.changed_shot, self.base_shots[1], self.changed_shot]
        if hit:
            return [self.changed_shot] * 3
        return list(self.base_shots[:3])

    def listing(self, offset):
        return 1, {i: f'rt 0x0 depth 0x0 vs 0x{i:08x} ps 0x0 prim TriangleList count 3' for i in range(self.n)}

    def dump_lines(self, offset, vs):
        return [f'Draw bisect: frame 1 draw 0 dump vs {vs}: ud ...']


def selftest():
    shots = ROOT / 'out/macos_route/20261011-104536-1080p-F3-P17-flip-retire/shots'
    base = sorted(str(p) for p in shots.glob('*on-p*.jpg'))[:3]
    other = str(next(shots.parent.parent.glob('20261011-110824-1080p-B59OFF-P17/shots/*idle-10.jpg')))
    if len(base) < 3:
        raise SystemExit('selftest needs the F3-P17 captures')
    roi = (0.25, 0.10, 0.70, 0.45)
    out = Path('/tmp/draw_bisect_selftest')
    results = []
    for bad, unclear in (([0], ()), ([737], ()), ([1478], ()), ([700, 701], ()), ([900], (900,))):
        fake = Fake(bad, 1479, base, other, unclear)
        b = Bisect(fake, roi, out / f'bad-{"-".join(map(str, bad))}')
        report = b.run()
        steps = len(report['steps'])
        if unclear:  # must not guess: it stops and leaves the case for review
            ok = not report['found'] and bool(report['review'])
        elif len(bad) == 1:
            ok = report['found'] == bad and report['draws'] and report['draws'][0]['skip_vs_changed']
        else:
            ok = bool(set(bad) & set(report['found']))
        results.append((bad, report['found'][:4], steps, len(report['review']), ok))
    for bad, found, steps, review, ok in results:
        print(f'selftest bad={bad}: found {found} in {steps} steps, {review} for review -> {"OK" if ok else "FAIL"}')
    return all(ok for *_, ok in results)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--run')
    p.add_argument('--roi', help='X0,Y0,X1,Y1 as fractions of the window')
    p.add_argument('--label', default='defect')
    p.add_argument('--captures', type=int, default=3)
    p.add_argument('--selftest', action='store_true')
    args = p.parse_args()
    source = ROOT / 'tools/macos/window_tool.swift'
    if not TOOL.exists() or TOOL.stat().st_mtime < source.stat().st_mtime:
        TOOL.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(['swiftc', '-O', '-o', str(TOOL), str(ROOT / 'tools/macos/window_tool.swift')], check=True)
    if args.selftest:
        sys.exit(0 if selftest() else 1)
    if not args.run or not args.roi:
        p.error('--run and --roi are required')
    roi = tuple(float(v) for v in args.roi.split(','))
    game = Game(args.run, args.captures)
    Bisect(game, roi, Path(args.run) / f'bisect-{args.label}').run()


if __name__ == '__main__':
    main()
