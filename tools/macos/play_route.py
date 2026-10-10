#!/usr/bin/env python3
"""Production playtest runs of the macOS build (x86_64 under Rosetta 2, MoltenVK).

Runs the signed bb-probe and its gpu/libbbgpu.dylib through run.sh exactly as shipped: no
DYLD_* overrides, no private libraries, no inherited BB_* switches. Each run gets a fresh user
directory (empty shader cache, no saves), scripted pad input (BB_PAD_FILE) and its own log,
screenshots and frame statistics under out/macos_route/<run>/. Any failed check stops with a
message; nothing is skipped quietly.

  play_route.py launch --probe PATH [--output 1080p|4k]   start a run, return once it logs
  play_route.py press TOKENS [SECONDS]                     hold pad tokens, then release
  play_route.py step NAME                                  a named menu step, then a capture
  play_route.py capture STAGE                              window screenshot + image/frame stats
  play_route.py stats [--since-stage STAGE]                frame statistics summary
  play_route.py status | stop
  play_route.py route --probe PATH [--output ...] [--until STAGE]   unattended route

Pad tokens are BB_PAD_FILE tokens (src/runtime_pad.c): cross circle square triangle l1 r1 l2 r2
l3 r3 options touchpad up down left right, sticks lx= ly= rx= ry= (0..255, 128 neutral).
Rosetta rules (team brief): at most two translated processes; no debugger or sampler is used.
"""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
BASE = ROOT / 'out' / 'macos_route'
DATA = BASE / 'data'  # run.sh generated files (boot images, patches), shared by all runs
TOOL_SOURCE = Path(__file__).resolve().parent / 'window_tool.swift'
TOOL = BASE / 'bin' / 'window_tool'
DEFAULT_GAME = Path.home() / 'CUSA03173'
DEFAULT_DEPS = Path.home() / 'bbport-deps' / 'x86_64'
ALLOWED_ENV = {'BB_GAME_DIR', 'BB_DEPS_PREFIX'}
SETTINGS = {
    # Native 1920x1080 output, the game's own size: FSR 3.1 native AA (the default preset).
    '1080p': 'upscaler=fsr3\npreset=0\noutput_res=1920x1080\nlive_resolution=0\n',
    # 3840x2160 output, FSR 3.1 Quality: the scene renders at 2560x1440 (startup patch).
    '4k': 'upscaler=fsr3\npreset=1\noutput_res=3840x2160\nlive_resolution=0\n',
}
FATAL = re.compile(r'Unimplemented PM4|Assertion|assert(ion)? failed|FATAL|Fatal|Unhandled|'
                   r'Segmentation|SIGSEGV|SIGBUS|SIGILL|abort|watchdog|^STOP:', re.I)
STATS = re.compile(r'^Frame stats: ([\d.]+) FPS, worst frame ([\d.]+) ms .*?; (\d+) shader/pipeline '
                   r'compiles, ([\d.]+) ms;.*?GPU thread ([\d.]+) us/draw, (\d+) draws/frame')
PACING = re.compile(r'^Frame pacing: median ([\d.]+) ms, stddev ([\d.]+) ms, p99 ([\d.]+) ms, '
                    r'(\d+) frames over')
# Single menu steps (title highlights Play Online first). Each token is one short press.
STEPS = {
    'offline': ['down', 'cross'],
    'new-game': ['cross'],
    'brightness': ['down', 'cross'],
    'controls': ['down'] * 6 + ['cross'],
    'skip-movie': ['options'],
    'confirm': ['cross'],
    'menu': ['options'],
    'back': ['circle'],
}
# The unattended route, as observed on c436893 (1080p, 2026-10-10). Operations:
#   ('wait_log', regex, timeout)              a log line since the last press matches
#   ('wait_draws', minimum, windows, timeout) that many frame-stats windows since the last press with at least
#                                             minimum draws/frame in a row
#   ('press', tokens) / ('sleep', seconds)
#   ('capture', stage, min_nonblack)          screenshot; fails when the window is darker
#   ('check_user_cache',)                     the GPU pipeline cache is inside the run's --user
# A stage that times out, or a game that exits or logs a fatal line, stops the route loudly.
ROUTE = [
    ('title', [('wait_log', r'^Runtime: pad opened', 120), ('wait_draws', 14, 3, 240), ('sleep', 3),
               ('capture', 'title', 0.2), ('check_user_cache',)]),
    ('offline', [('press', 'down'), ('press', 'cross'), ('sleep', 4), ('capture', 'offline-menu', 0.2)]),
    ('new-game', [('press', 'cross'), ('sleep', 5), ('capture', 'brightness', 0.05)]),
    ('brightness', [('press', 'down'), ('press', 'cross'), ('sleep', 3), ('capture', 'controls', 0.05)]),
    ('controls', [('press', 'down')] * 6 + [('capture', 'controls-next', 0.05), ('press', 'cross'),
                  ('wait_log', r'Init: Opening .*sprj_opening\.mp4', 60), ('sleep', 15),
                  ('capture', 'opening-movie', 0.05)]),
    ('movie-end', [('wait_log', r'DemuxerThread: Demuxer Thread exited normally', 300), ('sleep', 5),
                   ('capture', 'after-movie', None)]),
    ('character-creation', [('wait_draws', 100, 2, 300), ('sleep', 5), ('capture', 'character-creation', 0.05)]),
    # The cursor starts on Enter Name; BB_IME_TEXT fills the dialog without a keyboard.
    ('name', [('press', 'cross'), ('wait_log', r'^Runtime: ImeDialog text: ', 30), ('sleep', 2),
              ('capture', 'name-entered', 0.05)]),
    # Up wraps to Finish; the confirmation highlights NO, so left selects YES.
    ('finish', [('press', 'up'), ('capture', 'finish-selected', 0.05), ('press', 'cross'), ('sleep', 2),
                ('capture', 'finalise-prompt', 0.05), ('press', 'left'), ('press', 'cross'), ('sleep', 10),
                ('capture', 'contract-finalised', None)]),
]


def die(message):
    sys.exit('play_route: ' + message)


def now():
    return datetime.datetime.now(datetime.timezone.utc).isoformat(timespec='milliseconds')


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def run_checked(cmd, what):
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode:
        die(f'{what} failed ({result.returncode}): {" ".join(map(str, cmd))}\n{result.stdout}{result.stderr}')
    return result.stdout + result.stderr


def window_tool():
    """Builds the native helper on first use (or when its source changed)."""
    if not TOOL.exists() or TOOL.stat().st_mtime < TOOL_SOURCE.stat().st_mtime:
        TOOL.parent.mkdir(parents=True, exist_ok=True)
        run_checked(['swiftc', '-O', '-o', str(TOOL), str(TOOL_SOURCE)], 'building window_tool')
    return TOOL


# ---------------------------------------------------------------- runs and their records

def current_run(arg=None):
    run = Path(arg) if arg else BASE / 'current'
    if not (run / 'manifest.json').exists():
        die(f'no run at {run} (launch one first)')
    return run.resolve()


def manifest(run):
    return json.loads((run / 'manifest.json').read_text())


def event(run, kind, **data):
    data = {'time': now(), 'kind': kind, 'load_average': os.getloadavg(), **data}
    with (run / 'events.jsonl').open('a') as f:
        f.write(json.dumps(data) + '\n')
    return data


def alive(pid):
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    return True


def game_pid(run):
    pid = manifest(run).get('pid')
    if not pid or not alive(pid):
        exit_info = run / 'exit.json'
        detail = exit_info.read_text().strip() if exit_info.exists() else 'no exit record'
        die(f'game is not running ({detail}); log {run / "game.log"}')
    return pid


def log_lines(run):
    path = run / 'game.log'
    return path.read_text(errors='replace').splitlines() if path.exists() else []


def frame_stats(lines):
    samples = []
    for i, line in enumerate(lines):
        m = STATS.match(line)
        if m:
            fps, worst, compiles, compile_ms, us_draw, draws = m.groups()
            sample = {'line': i + 1, 'fps': float(fps), 'worst_ms': float(worst), 'compiles': int(compiles),
                      'compile_ms': float(compile_ms), 'us_per_draw': float(us_draw), 'draws': int(draws)}
            if i + 1 < len(lines) and (p := PACING.match(lines[i + 1])):
                sample.update(median_ms=float(p[1]), stddev_ms=float(p[2]), p99_ms=float(p[3]),
                              slow_frames=int(p[4]))
            samples.append(sample)
    return samples


def summarize(samples):
    if not samples:
        return {'windows': 0}
    fps = sorted(s['fps'] for s in samples)
    return {'windows': len(samples), 'fps_min': fps[0], 'fps_median': fps[len(fps) // 2], 'fps_max': fps[-1],
            'worst_ms': max(s['worst_ms'] for s in samples), 'compiles': sum(s['compiles'] for s in samples),
            'draws_max': max(s['draws'] for s in samples), 'last': samples[-1]}


def fatal_lines(lines):
    return [(i + 1, line) for i, line in enumerate(lines) if FATAL.search(line)]


# ---------------------------------------------------------------- launch

def verify_artifacts(probe):
    """The signed x86_64 production pair: bb-probe and gpu/libbbgpu.dylib next to it."""
    probe = probe.resolve()
    files = {'bb-probe': probe, 'libbbgpu.dylib': probe.parent / 'gpu' / 'libbbgpu.dylib'}
    caps = probe.parent / 'bb-gpu-capabilities'
    if caps.exists():
        files['bb-gpu-capabilities'] = caps
    record = {}
    for name, path in files.items():
        if not path.is_file():
            die(f'missing {name}: {path}')
        archs = run_checked(['lipo', '-archs', str(path)], f'lipo {name}').split()
        if archs != ['x86_64']:
            die(f'{name} must be x86_64 only, found {archs}: {path}')
        run_checked(['codesign', '--verify', '--strict', str(path)], f'codesign --verify {name}')
        info = run_checked(['codesign', '-dv', str(path)], f'codesign -dv {name}')
        cdhash = re.search(r'CDHash=(\w+)', info)
        record[name] = {'path': str(path), 'sha256': sha256(path), 'cdhash': cdhash[1] if cdhash else None}
    linked = run_checked(['otool', '-L', str(probe)], 'otool -L bb-probe')
    if '@rpath/libbbgpu.dylib' not in linked:
        die('bb-probe does not link @rpath/libbbgpu.dylib')
    source = subprocess.run(['git', '-C', str(probe.parent), 'rev-parse', 'HEAD'], capture_output=True, text=True)
    dirty = subprocess.run(['git', '-C', str(probe.parent), 'status', '--porcelain', '--untracked-files=no'],
                           capture_output=True, text=True)
    record['source_tree'] = {'head': source.stdout.strip() or None,
                             'tracked_changes': dirty.stdout.splitlines() if source.returncode == 0 else None}
    return record


def pin_artifacts(record):
    """Copies the verified files so a rebuild of the shared tree cannot change a running game.
    The copies are byte-identical (checked) and keep their embedded signatures."""
    key = hashlib.sha256((record['bb-probe']['sha256'] + record['libbbgpu.dylib']['sha256']).encode()).hexdigest()
    pinned = BASE / 'artifacts' / key[:16]
    targets = {'bb-probe': pinned / 'bb-probe', 'libbbgpu.dylib': pinned / 'gpu' / 'libbbgpu.dylib',
               'bb-gpu-capabilities': pinned / 'bb-gpu-capabilities'}
    for name, target in targets.items():
        if name not in record:
            continue
        if not target.exists():
            target.parent.mkdir(parents=True, exist_ok=True)
            tmp = target.with_suffix('.tmp')
            shutil.copy2(record[name]['path'], tmp)
            tmp.rename(target)
        if sha256(target) != record[name]['sha256']:
            die(f'pinned {name} differs from the verified file: {target}')
        run_checked(['codesign', '--verify', '--strict', str(target)], f'codesign --verify pinned {name}')
    return pinned / 'bb-probe'


def translated_games():
    out = subprocess.run(['pgrep', '-x', 'bb-probe'], capture_output=True, text=True).stdout.split()
    return [int(p) for p in out]


def launch(args):
    if args.perf_phases and not args.perf:
        die('--perf-phases requires --perf')
    dyld = sorted(k for k in os.environ if k.startswith('DYLD_'))
    if dyld:
        die('refusing to launch with library overrides (unset them): ' + ', '.join(dyld))
    # Inherited BB_* names (the bb IDE's own, or game switches) never reach the game.
    removed = sorted(k for k in os.environ if k.startswith('BB_') and k not in ALLOWED_ENV)
    if removed:
        print('play_route: not passed to the game: ' + ', '.join(removed), file=sys.stderr)
    running = translated_games()
    if len(running) >= 2:
        die(f'two bb-probe processes are already running ({running}); the Rosetta limit is two')
    game = Path(os.environ.get('BB_GAME_DIR', DEFAULT_GAME))
    deps = Path(os.environ.get('BB_DEPS_PREFIX', DEFAULT_DEPS))
    if not (game / 'eboot.bin').is_file():
        die(f'no eboot.bin in {game} (set BB_GAME_DIR)')
    if not (deps / 'share/vulkan/icd.d/MoltenVK_icd.json').is_file():
        die(f'no MoltenVK ICD under {deps} (set BB_DEPS_PREFIX)')
    artifacts = verify_artifacts(Path(args.probe))
    probe = pin_artifacts(artifacts)

    stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    run = BASE / f'{stamp}-{args.output}{"-" + args.label if args.label else ""}'
    run.mkdir(parents=True)
    (run / 'user').mkdir()
    (run / 'shots').mkdir()
    (run / 'pad.txt').write_text('\n')
    (run / 'bbport.ini').write_text(SETTINGS[args.output])
    DATA.mkdir(parents=True, exist_ok=True)
    env = {k: v for k, v in os.environ.items() if k not in removed}
    env.update(BB_PREBUILT='1', BB_PROBE=str(probe), BB_GAME_DIR=str(game), BB_DEPS_PREFIX=str(deps),
               BB_DATA_DIR=str(DATA), BB_USER_DIR=str(run / 'user'),
               BB_CONFIG=str(run / 'bbport.ini'),
               BB_PAD_FILE=str(run / 'pad.txt'), BB_IME_TEXT=args.name, BB_FPS=args.fps,
               BB_FRAME_STATS='1', BB_MODS_ENABLED='0', BB_TIMEOUT=str(args.timeout))
    if args.gpu_log:
        env['BB_GPU_LOG'] = args.gpu_log
    if args.perf:
        env.update(BB_PERF='1', BB_PERF_OUT=str(run / 'perf.jsonl'))  # the library creates it (fails if present)
    if args.perf_phases:
        env['BB_PERF_PHASES'] = '1'  # one submit per phase: perturbs timing, never an FPS measurement
    if args.dump_shaders:
        env['BB_DUMP_SHADERS'] = '1'
    for item in args.diag_env:
        key, sep, value = item.partition('=')
        if not sep or not key.startswith('BB_') or key in env:
            die(f'--diag-env needs a new BB_ name and a value: {item}')
        env[key] = value
    if args.disable_optimizations is not None:
        # Diagnostic A/B: optimizations switched off from the start (src/runtime_memory.c bits).
        (run / 'toggles').write_text(f'{args.disable_optimizations}\n')
        env['BB_TOGGLE_FILE'] = str(run / 'toggles')
    if args.frame_capture:
        (run / 'captures').mkdir()
        env.update(BB_CAPTURE_TRIGGER=str(run / 'capture.trigger'), BB_CAPTURE_DIR=str(run / 'captures'))
    record = {'run': run.name, 'started': now(), 'output': args.output, 'settings': SETTINGS[args.output],
              'artifacts': artifacts, 'pinned_probe': str(probe), 'repo_head': subprocess.run(
                  ['git', '-C', str(ROOT), 'rev-parse', 'HEAD'], capture_output=True, text=True).stdout.strip(),
              'env': {k: v for k, v in env.items() if k.startswith(('BB_', 'DYLD_'))}, 'env_removed': removed,
              'diagnostics': [n for n, on in (('BB_DUMP_SHADERS', args.dump_shaders),
                                              ('BB_CAPTURE_TRIGGER', args.frame_capture),
                                              ('BB_TOGGLE_FILE', args.disable_optimizations is not None)) if on]
                             + args.diag_env,
              'disabled_optimizations': args.disable_optimizations,
              'perf': args.perf, 'perf_phases': args.perf_phases,
              'shader_cache': 'empty (fresh user directory)', 'saves': 'none (fresh user directory)',
              'host': {'load_average': os.getloadavg(), 'other_bb_probe': running}}
    (run / 'manifest.json').write_text(json.dumps(record, indent=2) + '\n')
    current = BASE / 'current'
    if current.is_symlink() or current.exists():
        current.unlink()
    current.symlink_to(run.name)
    # A detached supervisor owns the game, so its exit status is recorded however the run ends.
    subprocess.Popen([sys.executable, __file__, '_supervise', str(run)], start_new_session=True,
                     stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env)
    deadline = time.time() + 60
    while time.time() < deadline:
        time.sleep(1)
        info = manifest(run)
        if (run / 'exit.json').exists():
            die(f'game exited during startup: {(run / "exit.json").read_text().strip()}; log {run / "game.log"}')
        if info.get('pid') and any(l.startswith('Runtime:') for l in log_lines(run)):
            print(json.dumps({'run': str(run), 'pid': info['pid'], 'log': str(run / 'game.log')}))
            return run
    die(f'no runtime output within 60 s (Rosetta rule: report, do not retry); run {run}')


def supervise(run):
    with (run / 'game.log').open('wb') as log:
        p = subprocess.Popen(['/bin/bash', str(ROOT / 'run.sh')], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT,
                             stdin=subprocess.DEVNULL)
        info = manifest(run)
        info['pid'] = p.pid  # run.sh execs bb-probe, so this is the game's pid
        (run / 'manifest.json').write_text(json.dumps(info, indent=2) + '\n')
        code = p.wait()
    (run / 'exit.json').write_text(json.dumps({'time': now(), 'returncode': code}) + '\n')


# ---------------------------------------------------------------- input and observation

def press(run, tokens, seconds=0.12, gap=0.8):
    game_pid(run)
    pad = run / 'pad.txt'
    pad.write_text(tokens + '\n')
    time.sleep(seconds)
    pad.write_text('\n')
    event(run, 'press', tokens=tokens, seconds=seconds)
    time.sleep(gap)


def capture(run, stage, min_nonblack=None):
    pid = game_pid(run)
    tool = window_tool()
    window = subprocess.run([str(tool), 'window', str(pid)], capture_output=True, text=True)
    if window.returncode:
        die('no game window: ' + window.stderr.strip())
    shots = sorted((run / 'shots').glob('*.jpg'))
    target = run / 'shots' / f'{len(shots):02d}-{stage}.png'
    run_checked(['/usr/sbin/screencapture', '-x', '-o', '-l', window.stdout.strip(), str(target)], 'screencapture')
    image = json.loads(run_checked([str(tool), 'stats', str(target)], 'image stats'))
    # The statistics use the full-resolution capture; the kept copy is a 1512-px JPEG (~0.2 MB).
    jpeg = target.with_suffix('.jpg')
    run_checked(['sips', '-s', 'format', 'jpeg', '-s', 'formatOptions', '85', '-Z', '1512', str(target),
                 '--out', str(jpeg)], 'screenshot conversion')
    target.unlink()
    target = jpeg
    lines = log_lines(run)
    samples = frame_stats(lines)
    data = event(run, 'capture', stage=stage, path=str(target), image=image, log_lines=len(lines),
                 frame_stats=samples[-3:], fatal=fatal_lines(lines)[-5:])
    print(json.dumps({'stage': stage, 'path': str(target), 'image': image,
                      'frame_stats': samples[-1] if samples else None, 'fatal': data['fatal']}))
    if min_nonblack is not None and image['nonblack'] < min_nonblack:
        die(f'stage {stage}: window is {image["nonblack"]:.4f} non-black (< {min_nonblack}); {target}')
    return data


def stop(run):
    info = manifest(run)
    pid = info.get('pid')
    if pid and alive(pid):
        os.kill(pid, signal.SIGTERM)
        for _ in range(30):
            if not alive(pid):
                break
            time.sleep(1)
        else:
            die(f'pid {pid} still running 30 s after SIGTERM')
    event(run, 'stop', pid=pid)
    print(json.dumps({'stopped': pid, 'exit': json.loads((run / 'exit.json').read_text())
                      if (run / 'exit.json').exists() else None}))


def status(run):
    info = manifest(run)
    lines = log_lines(run)
    print(json.dumps({'run': str(run), 'pid': info.get('pid'), 'alive': bool(info.get('pid') and alive(info['pid'])),
                      'exit': json.loads((run / 'exit.json').read_text()) if (run / 'exit.json').exists() else None,
                      'log_lines': len(lines), 'frame_stats': summarize(frame_stats(lines)),
                      'fatal': fatal_lines(lines)[-5:]}, indent=2))


# ---------------------------------------------------------------- unattended route

def check_running(run, since):
    if (run / 'exit.json').exists():
        die(f'game exited: {(run / "exit.json").read_text().strip()}; log {run / "game.log"}')
    bad = [(n, l) for n, l in fatal_lines(log_lines(run)) if n > since]
    if bad:
        die(f'fatal log line {bad[0][0]}: {bad[0][1]}')


def wait_until(run, timeout, what, done, start_line):
    deadline = time.time() + timeout
    while time.time() < deadline:
        lines = log_lines(run)
        if done(lines, start_line):
            return
        check_running(run, start_line)
        time.sleep(2)
    die(f'timed out after {timeout} s waiting for {what}')


def route(run, until, hold=0):
    names = [name for name, _ in ROUTE]
    if until and until not in names:
        die('known stages: ' + ', '.join(names))
    mark = len(log_lines(run))  # waits look at log lines from here: set before every press
    for name, ops in ROUTE:
        event(run, 'stage', stage=name)
        print(f'play_route: stage {name}', file=sys.stderr, flush=True)
        for op in ops:
            if op[0] == 'wait_log':
                pattern = re.compile(op[1])
                wait_until(run, op[2], op[1], lambda lines, since: any(pattern.search(l) for l in lines[since:]), mark)
            elif op[0] == 'wait_draws':
                def enough(lines, since, minimum=op[1], count=op[2]):
                    streak = 0
                    for sample in frame_stats(lines):
                        if sample['line'] > since:
                            streak = streak + 1 if sample['draws'] >= minimum else 0
                    return streak >= count
                wait_until(run, op[3], f'{op[2]} windows with >= {op[1]} draws/frame', enough, mark)
            elif op[0] == 'press':
                mark = len(log_lines(run))
                press(run, op[1])
            elif op[0] == 'sleep':
                time.sleep(op[1])
            elif op[0] == 'capture':
                capture(run, op[1], op[2])
            elif op[0] == 'check_user_cache':
                cache = run / 'user' / 'cache'
                if not cache.is_dir() or not any(cache.iterdir()):
                    die(f'the GPU pipeline cache is not under --user ({cache} missing or empty)')
                event(run, 'check', user_cache=str(cache))
            check_running(run, 0)
        if name == until:
            break
    if hold:
        # Stay on the last screen without input, so frame statistics cover it (perf windows).
        event(run, 'hold', seconds=hold)
        deadline = time.time() + hold
        while time.time() < deadline:
            check_running(run, 0)
            time.sleep(2)
    event(run, 'route-complete', until=until)
    print(json.dumps({'route': 'complete', 'until': until, 'frame_stats': summarize(frame_stats(log_lines(run)))}))


# ---------------------------------------------------------------- command line

def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--run', help='run directory (default out/macos_route/current)')
    sub = parser.add_subparsers(dest='command', required=True)
    for command in ('launch', 'route'):
        p = sub.add_parser(command)
        p.add_argument('--probe', required=True, help='signed production bb-probe (gpu/libbbgpu.dylib beside it)')
        p.add_argument('--output', choices=sorted(SETTINGS), default='1080p')
        p.add_argument('--label', default='')
        p.add_argument('--name', default='Hunter', help='BB_IME_TEXT: the text every IME dialog accepts')
        p.add_argument('--fps', default='60', help='BB_FPS frame-rate patch')
        p.add_argument('--timeout', type=int, default=3600, help='BB_TIMEOUT seconds')
        p.add_argument('--gpu-log', help='BB_GPU_LOG level (default: the build default)')
        p.add_argument('--frame-capture', action='store_true', help='arm BB_CAPTURE_TRIGGER (frame analyzer)')
        p.add_argument('--dump-shaders', action='store_true', help='BB_DUMP_SHADERS=1 (diagnostic: writes every shader)')
        p.add_argument('--diag-env', action='append', default=[], metavar='BB_NAME=VALUE',
                       help='extra diagnostic BB_ variable (recorded; repeatable)')
        p.add_argument('--disable-optimizations', type=int, metavar='MASK',
                       help='BB_TOGGLE_FILE with this decimal mask from the start (diagnostic A/B)')
        p.add_argument('--perf', action='store_true', help='BB_PERF=1, BB_PERF_OUT=<run>/perf.jsonl')
        p.add_argument('--perf-phases', action='store_true',
                       help='BB_PERF_PHASES=1 (needs --perf; perturbed timing, not for FPS claims)')
        if command == 'route':
            p.add_argument('--until', help='last stage to run: ' + ', '.join(name for name, _ in ROUTE))
            p.add_argument('--keep', action='store_true', help='leave the game running after the route')
            p.add_argument('--hold', type=int, default=0, metavar='SECONDS',
                           help='after the last stage, stay idle this long (fails loudly if the game dies)')
    p = sub.add_parser('press')
    p.add_argument('tokens')
    p.add_argument('seconds', type=float, nargs='?', default=0.12)
    p = sub.add_parser('step')
    p.add_argument('name', choices=sorted(STEPS))
    p = sub.add_parser('capture')
    p.add_argument('stage')
    p.add_argument('--min-nonblack', type=float)
    sub.add_parser('frame-capture')
    sub.add_parser('stats')
    sub.add_parser('status')
    sub.add_parser('stop')
    p = sub.add_parser('_supervise')
    p.add_argument('dir')
    args = parser.parse_args()

    if args.command == 'launch':
        launch(args)
    elif args.command == 'route':
        run = launch(args)
        try:
            route(run, args.until, args.hold)
        finally:
            if not args.keep:
                stop(run)
    elif args.command == '_supervise':
        supervise(Path(args.dir))
    else:
        run = current_run(args.run)
        if args.command == 'press':
            press(run, args.tokens, args.seconds)
        elif args.command == 'step':
            for token in STEPS[args.name]:
                press(run, token)
            time.sleep(1)
            capture(run, args.name)
        elif args.command == 'capture':
            capture(run, args.stage, args.min_nonblack)
        elif args.command == 'frame-capture':
            if 'BB_CAPTURE_TRIGGER' not in manifest(run)['env']:
                die('this run was launched without --frame-capture')
            (run / 'capture.trigger').touch()
            event(run, 'frame-capture')
        elif args.command == 'stats':
            print(json.dumps(summarize(frame_stats(log_lines(run))), indent=2))
        elif args.command == 'status':
            status(run)
        elif args.command == 'stop':
            stop(run)


if __name__ == '__main__':
    main()
