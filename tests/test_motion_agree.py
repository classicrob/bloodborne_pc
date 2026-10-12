#!/usr/bin/env python3
"""tools/macos/motion_agree.py on synthetic dumps: its verdicts and refusals (exit codes).

A unittest (build.sh --test discovers tests/test_*.py) and a script (prints each case)."""
import json
import os
import subprocess
import sys
import tempfile
import unittest

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPT = os.path.join(ROOT, 'tools', 'macos', 'motion_agree.py')
W, H = 64, 48


def dump(directory, frame=0, session=1, camera_only=True, mode=0, flip=1.0, object_valid=True,
         motion_size=(W, H), extra_objects=False, short_depth=False, no_motion=False):
    prefix = f'f{frame:03d}'
    with open(os.path.join(directory, f'{prefix}_meta.json'), 'w') as f:
        json.dump({'session': session, 'frame': frame, 'motion_size': list(motion_size),
                   'camera_only': camera_only, 'mode': mode, 'object_valid': object_valid,
                   'proj_z': 1.0}, f)
    depth = np.full((H, W), 0.9, np.float32)
    obj = np.zeros((H, W, 4), np.float32)
    obj[..., 0], obj[..., 1], obj[..., 2], obj[..., 3] = 0.4, 1.5, 1.0, 0.9
    camera = np.zeros((H, W, 2), np.float16)
    camera[..., 0], camera[..., 1] = 0.4, 1.5 * flip
    if not no_motion:  # a pending or failed write
        camera.tofile(os.path.join(directory, f'{prefix}_motion_{W}x{H}_rg16f.raw'))
    obj.tofile(os.path.join(directory, f'{prefix}_objects_{W}x{H}_rgba32f.raw'))
    if extra_objects:  # an older dump of another size, same frame number
        np.zeros((24, 32, 4), np.float32).tofile(
            os.path.join(directory, f'{prefix}_objects_32x24_rgba32f.raw'))
    (depth[:-1] if short_depth else depth).tofile(
        os.path.join(directory, f'{prefix}_depth_{W}x{H}_f32.raw'))


def expect(label, expected, frames=({},), options=(), directory_name='session-1'):
    """Runs one case, prints it; True when the exit code is the expected one."""
    with tempfile.TemporaryDirectory() as parent:
        directory = os.path.join(parent, directory_name)
        os.mkdir(directory)
        for spec in frames:
            dump(directory, **spec)
        result = subprocess.run([sys.executable, SCRIPT, directory, *options],
                                capture_output=True, text=True)
    ok = result.returncode == expected
    last = result.stdout.strip().splitlines()[-1] if result.stdout.strip() else ''
    print(f'{label:58s} exit {result.returncode} (expected {expected}) '
          f'{"ok" if ok else "FAIL"}: {last}')
    return ok


def run_all():
    """Every case; the number that failed."""
    failures = 0
    failures += not expect('camera agrees with object vectors', 0)
    failures += not expect('camera y flipped', 1, frames=[{'flip': -1.0}])
    failures += not expect('ordinary merged dump (not camera-only)', 3, frames=[{'camera_only': False, 'mode': 1}])
    failures += not expect('camera-only flag but merged mode', 3, frames=[{'mode': 1}])
    failures += not expect('ambiguous companion (two objects files)', 3, frames=[{'extra_objects': True}])
    failures += not expect('malformed companion (short depth)', 3, frames=[{'short_depth': True}])
    failures += not expect('motion size differs from the files', 3, frames=[{'motion_size': (32, 24)}])
    failures += not expect('meta session differs from its session directory', 3,
           frames=[{'frame': 0, 'session': 1}, {'frame': 1, 'session': 2}])
    failures += not expect('new session with a pending (missing) motion write', 3,
           frames=[{'frame': 0}, {'frame': 1, 'no_motion': True}])
    failures += not expect('not a session-<id> directory', 3, directory_name='dump')
    failures += not expect('malformed motion_size', 3, frames=[{'motion_size': (W,)}])
    for option, value in (('--max-median', 'inf'), ('--max-median', 'nan'), ('--max-median', '-1'),
                          ('--min-vertical', 'nan'), ('--min-vertical', '-0.5')):
        failures += not expect(f'invalid threshold {option} {value} (y flipped)', 3, frames=[{'flip': -1.0}],
               options=(option, value))
    failures += not expect('no object vectors: unjudged', 2, frames=[{'object_valid': False}])
    with tempfile.TemporaryDirectory() as parent:
        empty = os.path.join(parent, 'session-7')
        os.mkdir(empty)
        result = subprocess.run([sys.executable, SCRIPT, empty], capture_output=True, text=True)
        ok = result.returncode == 3
        failures += not ok
        print(f'{"no meta (old build or no dump)":58s} exit {result.returncode} (expected 3) '
              f'{"ok" if ok else "FAIL"}')
    return failures


class MotionAgreeTest(unittest.TestCase):
    def test_verdicts_and_refusals(self):
        self.assertEqual(run_all(), 0)


if __name__ == '__main__':
    failed = run_all()
    print(f'motion-agree-test: {"FAIL" if failed else "PASS"}')
    sys.exit(1 if failed else 0)
