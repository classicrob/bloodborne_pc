#!/usr/bin/env python3
"""Replay A/B from existing build manifests. Inputs are never built, moved or deleted.

Usage: replay-ab.sh A_MANIFEST B_MANIFEST --capture DIR [--data DIR] [--iters 30]
Requires source_tree.head and artifacts.{bb-probe,libbbgpu.dylib}.{path,sha256}.
Shared preview manifests require qualification.test_gate=PASS. Historical attribution manifests
with a missing/NOT_RUN/SKIPPED gate require --qualification-only and remain unqualified.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import time

from replay_identity import FILES, digest
from replay_summary import summarize

ROOT = Path(__file__).resolve().parents[2]
PROTECTED = {'BB_REPLAY_RUN', 'BB_REPLAY_ITERATIONS', 'BB_REPLAY_GPU_WAIT', 'BB_PERF', 'BB_PERF_OUT',
             'BB_CONFIG', 'VK_DRIVER_FILES', 'BB_WINDOW_SIZE', 'BB_SPLIT_WINDOW'}


def write_json(path, value):
    with path.open('x') as f:
        json.dump(value, f, indent=2); f.write('\n')


def build_manifest(path, qualification_only=False):
    value = json.loads(path.read_text())
    head = value['source_tree']['head'] if 'source_tree' in value else value['artifacts']['source_tree']['head']
    if not re.fullmatch('[0-9a-f]{40}', head):
        raise ValueError(f'{path}: full source SHA required')
    gate = value.get('qualification', {}).get('test_gate')
    if gate != 'PASS' and not (qualification_only and gate in (None, 'NOT_RUN', 'SKIPPED')):
        raise ValueError(f'{path}: preview test_gate must be PASS (got {gate})')
    artifacts = value['artifacts']
    result = {'head': head, 'manifest': str(path.resolve()), 'manifest_sha256': digest(path),
              'gate': gate or 'UNQUALIFIED SOURCE GATE', 'artifacts': {}}
    for name in ('bb-probe', 'libbbgpu.dylib'):
        item = artifacts[name]
        target = Path(item['path'])
        if not target.is_absolute() or not target.is_file() or digest(target) != item['sha256']:
            raise ValueError(f'{path}: {name} missing or SHA256 mismatch')
        result['artifacts'][name] = {'path': str(target.resolve()), 'sha256': item['sha256']}
    probe = Path(result['artifacts']['bb-probe']['path'])
    lib = Path(result['artifacts']['libbbgpu.dylib']['path'])
    if not os.access(probe, os.X_OK) or (probe.parent / 'gpu/libbbgpu.dylib').resolve() != lib:
        raise ValueError('executable probe must load the verified gpu/libbbgpu.dylib beside it')
    return result


def env_items(items):
    result = {}
    for item in items:
        for word in shlex.split(item):
            key, sep, value = word.partition('=')
            if not sep or not re.fullmatch('BB_[A-Z0-9_]+', key) or key in PROTECTED or key.startswith('BB_HEAVY_'):
                raise ValueError(f'unsupported or protected environment override: {key}')
            result[key] = value
    return result


def capture_inputs(capture):
    manifest = json.loads((capture / 'manifest.json').read_text())
    if capture.name.endswith('.incomplete') or manifest.get('manifest_version') != 2:
        raise ValueError('complete capture manifest_version 2 required')
    if set(manifest['files']) != set(FILES):
        raise ValueError('capture file set mismatch')
    with (capture / 'resets.bin').open('rb') as f:
        header = f.read(16)
    if header != b'BBRPRSET' + (3).to_bytes(4, 'little') + bytes(4):
        raise ValueError('named reset v3 required; legacy migration is explicit, never automatic')
    hashes = {'manifest.json': digest(capture / 'manifest.json')}
    for name in FILES:
        path = capture / name
        if path.stat().st_size != manifest['files'][name]:
            raise ValueError(f'capture {name}: size mismatch')
        hashes[name] = digest(path)
    return {'path': str(capture), 'sha256': hashes,
            'qualification': manifest.get('pixel_output', 'unqualified'),
            'identity_migration': manifest.get('identity_migration')}


def host_state():
    result = {'utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()), 'load': os.getloadavg()}
    for name, command in [('power', ['pmset', '-g', 'custom']), ('thermal', ['pmset', '-g', 'therm']),
                          ('memory', ['memory_pressure', '-Q']), ('top', ['ps', '-Ao', 'pcpu,pid,comm', '-r'])]:
        p = subprocess.run(command, capture_output=True, text=True, timeout=10)
        result[name] = {'status': p.returncode, 'stdout': p.stdout[:8192], 'stderr': p.stderr[:2048]}
    return result


def execute(plan_path):
    plan = json.loads(plan_path.read_text())
    write_json(Path(plan['host']), host_state())  # after heavy's quiet/settle, immediately before launch
    environment = {k: v for k, v in os.environ.items() if not k.startswith('BB_') and not k.startswith('DYLD_')}
    environment.update(plan['env'])
    return subprocess.call([sys.executable, str(ROOT / 'tools/macos/replay_watch.py'), plan['log'],
                            '60', '120', '--', *plan['command']], env=environment)


def main():
    if len(sys.argv) == 3 and sys.argv[1] == '--execute':
        return execute(Path(sys.argv[2]))
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('a', type=Path); p.add_argument('b', type=Path)
    p.add_argument('--capture', type=Path, required=True)
    p.add_argument('--data', type=Path, default=ROOT / 'out/macos_route/data-1080p')
    p.add_argument('--iters', type=int, default=30)
    p.add_argument('--order', default='ABBA')
    p.add_argument('--env', action='append', default=[])
    p.add_argument('--env-a', action='append', default=[]); p.add_argument('--env-b', action='append', default=[])
    p.add_argument('--settle', default='6,120')
    p.add_argument('--label', default=time.strftime('replay-ab-%Y%m%d-%H%M%S'))
    p.add_argument('--qualification-only', action='store_true', help='permit unexecuted/skipped source gates for correctness qualification; timings unqualified')
    args = p.parse_args()
    if not 3 <= args.iters <= 10000 or not re.fullmatch('[AB]+', args.order) or set(args.order) != {'A', 'B'}:
        raise ValueError('iters must be 3..10000; order must include both A and B')
    if not re.fullmatch('[A-Za-z0-9._-]+', args.label) or not re.fullmatch(r'[0-9]+(?:\.[0-9]+)?,[1-9][0-9]*', args.settle):
        raise ValueError('invalid label or settle LOAD,SECONDS')
    if shutil.disk_usage(ROOT).free < 45 * 1000 ** 3:
        raise ValueError('replay run budget requires 45 GB free: 35 floor + 5 buffer + 5 job budget')
    if os.environ.get('BB_HEAVY_HELD'):
        raise ValueError('start orchestration outside a heavy slot; each run acquires its own exclusive GPU/quiet slot')
    builds = {'A': build_manifest(args.a, args.qualification_only),
              'B': build_manifest(args.b, args.qualification_only)}
    common = env_items(args.env)
    overrides = {'A': env_items(args.env_a), 'B': env_items(args.env_b)}
    capture, data = args.capture.resolve(strict=True), args.data.resolve(strict=True)
    deps = Path(os.environ.get('BB_DEPS_PREFIX', str(Path.home() / 'bbport-deps/x86_64')))
    game = Path(os.environ.get('BB_GAME_DIR', str(Path.home() / 'CUSA03173')))
    icd = deps / 'share/vulkan/icd.d/MoltenVK_icd.json'
    if not icd.is_file() or not game.is_dir():
        raise ValueError('game or MoltenVK ICD missing')
    data_hashes = {name: digest(data / 'out' / name) for name in ('boot-linked.bin', 'content.bin', 'patches.bin')}
    logs = ROOT / 'out/replay-ab' / args.label
    logs.mkdir(parents=True, exist_ok=False)
    # This is a repeated-workload attribution config; it is never native-output acceptance.
    ini = logs / 'bbport.ini'
    ini.write_text('preset=0\noutput_res=1920x1080\nupscaler=off\nobject_motion=0\nlive_resolution=0\n')
    provenance = {'version': 1, 'qualification': 'repeated-workload relative attribution; pixels/live FPS unqualified',
                  'builds': builds, 'data_sha256': data_hashes, 'config_sha256': digest(ini),
                  'qualification_only': args.qualification_only, 'order': args.order, 'iterations': args.iters, 'env_common': common, 'env_arms': overrides}
    # Hash once in a build slot; full capture reads are outside all timed/quiet intervals.
    hash_path = logs / 'capture-inputs.json'
    command = ['bash', str(ROOT / 'tools/macos/heavy.sh'), '--pool', 'build', args.label + ' input hashes',
               '--', sys.executable, str(Path(__file__).resolve()), '--hash-capture', str(capture), str(hash_path)]
    if subprocess.call(command, cwd=ROOT):
        raise ValueError('capture input qualification/hash step failed')
    provenance['capture'] = json.loads(hash_path.read_text())
    write_json(logs / 'provenance.json', provenance)
    (logs / 'user').mkdir()
    results = []
    for number, arm in enumerate(args.order, 1):
        tag = f'{number}-{arm}'
        for artifact in builds[arm]['artifacts'].values():
            if digest(Path(artifact['path'])) != artifact['sha256']:
                raise ValueError('pinned build changed after verification')
        for name, expected in data_hashes.items():
            if digest(data / 'out' / name) != expected:
                raise ValueError('boot inputs changed during A/B')
        environment = {'VK_DRIVER_FILES': str(icd), 'BB_CONFIG': str(ini), 'BB_UPSCALER': 'none',
                       'BB_WINDOW_SIZE': '320x180', 'BB_WINDOW_BACKGROUND': '1', 'BB_WINDOW_POS': '0,0',
                       'BB_FRAME_STATS': '1', 'BB_GPU_LOG': 'info', 'BB_REPLAY_GPU_WAIT': '1', 'BB_PERF': '1',
                       'BB_PERF_OUT': str(logs / f'perf-{tag}.jsonl'), **common, **overrides[arm],
                       'BB_REPLAY_RUN': str(capture), 'BB_REPLAY_ITERATIONS': str(args.iters), 'BB_SPLIT_WINDOW': '1'}
        probe = builds[arm]['artifacts']['bb-probe']['path']
        plan = {'command': [probe, str(data / 'out/boot-linked.bin'), '--content-profile', str(data / 'out/content.bin'),
                            '--patches', str(data / 'out/patches.bin'), '--app0', str(game), '--user', str(logs / 'user'),
                            '--timeout', '900'], 'env': environment, 'log': str(logs / f'run-{tag}.log'),
                'host': str(logs / f'host-{tag}.json')}
        plan_path = logs / f'plan-{tag}.json'; write_json(plan_path, plan)
        heavy = ['bash', str(ROOT / 'tools/macos/heavy.sh'), '--pool', 'playtest', '--gpu', 'exclusive',
                 '--cpu-quiet=stop', '--settle', args.settle, args.label + ' run ' + tag, '--',
                 sys.executable, str(Path(__file__).resolve()), '--execute', str(plan_path)]
        with (logs / f'heavy-{tag}.log').open('x') as stream:
            status = subprocess.call(heavy, cwd=ROOT, stdout=stream, stderr=subprocess.STDOUT)
        if status:
            raise ValueError(f'run {tag} failed with exit {status}; see {logs}')
        if re.search(r'INVALID|CPU QUIET BOUND REACHED|identity unreadable|not resumed', (logs / f'heavy-{tag}.log').read_text()):
            raise ValueError(f'run {tag}: heavy qualification failed')
        result = summarize(Path(plan['log']), args.iters, True)
        result.update(arm=arm, run=number); results.append(result)
        write_json(logs / f'result-{tag}.json', result)
        print(f"{tag}: submit-to-idle median {result['median_ms']:.2f} ms; GPU busy {result['gpu_busy_median_ms']:.2f} ms", flush=True)
    write_json(logs / 'results.json', results)
    print(f'Attribution artifacts: {logs}; replay does not establish live FPS')
    return 0


if __name__ == '__main__':
    try:
        if len(sys.argv) == 4 and sys.argv[1] == '--hash-capture':
            write_json(Path(sys.argv[3]), capture_inputs(Path(sys.argv[2])))
            sys.exit(0)
        sys.exit(main())
    except (OSError, ValueError, KeyError, TypeError) as error:
        sys.exit(f'STOP: replay A/B: {error}')
