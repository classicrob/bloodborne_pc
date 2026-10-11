#!/usr/bin/env python3
"""Camera motion vectors against the GPU's own object vectors, from an upscaler dump.

Run the game with BB_MOTION_CAMERA_ONLY=1 BB_DUMP_TRIGGER=<file> BB_DUMP_DIR=<fresh dir>
(BB_DUMP_FRAMES=8 by default) and an upscaler on, create <file> while the camera pitches or moves
vertically (stairs), then:

    python3 tools/macos/motion_agree.py <dir> [--max-median 0.1]

Each dumped frame has fNNN_meta.json (written with the dump): the dump session, the motion size,
whether the motion target held camera vectors only (camera_only, mode 0), whether object vectors
were available, and the projection z scale the shader used. A frame is judged only when that
provenance says camera-only with object vectors, and its companions are unique and well formed:
exactly one fNNN_motion_WxH_rg16f, fNNN_objects_WxH_rgba32f and fNNN_depth_WxH_f32 of the meta's
size, with the right byte sizes. It then compares the camera vector with the object vector (rg) on
the pixels whose object vector is valid (b > 0.99) and drew the final depth (|a - depth| under the
shader's own threshold max(2e-7, (proj_z - depth) * 0.01)).

What a verdict says: the medians agree (or not) over THOSE pixels. It does not show that moving
geometry was excluded, nor that every static surface agrees: pick a view dominated by static
geometry, and read the p90 and the in-0.25-px share.

Exit: 0 every judged frame agrees; 1 a judged frame disagrees; 2 nothing could be judged (no
object vectors, too little vertical motion, too few pixels); 3 the dump is unusable (not
camera-only, mixed sessions, missing, ambiguous or malformed files), which is never a success.
"""
import argparse
import glob
import json
import math
import os
import re
import sys

import numpy as np


class Unusable(Exception):
    pass


def companion(directory, prefix, name, fmt, size, bytes_per_pixel):
    """The one fNNN_<name>_WxH_<fmt>.raw of this frame, of the meta's size, well formed."""
    found = glob.glob(os.path.join(directory, f'{prefix}_{name}_*x*_{fmt}.raw'))
    if len(found) != 1:
        raise Unusable(f'{prefix}: {len(found)} {name} files (expected exactly one)')
    match = re.search(r'_(\d+)x(\d+)_', os.path.basename(found[0]))
    width, height = int(match.group(1)), int(match.group(2))
    if (width, height) != size:
        raise Unusable(f'{prefix}: the {name} file is {width}x{height}, the meta motion_size '
                       f'{size[0]}x{size[1]}')
    expected = width * height * bytes_per_pixel
    if os.path.getsize(found[0]) != expected:
        raise Unusable(f'{prefix}: {name} has {os.path.getsize(found[0])} bytes, not {expected}')
    return found[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('dump_dir')
    parser.add_argument('--max-median', type=float, default=0.1,
                        help='largest median |camera - object| in px that counts as agreement')
    parser.add_argument('--min-vertical', type=float, default=0.5,
                        help='median |object dy| in px a frame needs to judge the y convention')
    args = parser.parse_args()

    metas = sorted(glob.glob(os.path.join(args.dump_dir, 'f*_meta.json')))
    if not metas:
        print(f'unusable: no fNNN_meta.json in {args.dump_dir} (a dump from a build without '
              'motion provenance, or no dump)')
        return 3
    try:
        loaded = []
        for path in metas:
            with open(path) as f:
                loaded.append((path, json.load(f)))
        sessions = {meta['session'] for _, meta in loaded}
        if len(sessions) != 1:
            raise Unusable(f'{len(sessions)} dump sessions in one directory: use a fresh '
                           'BB_DUMP_DIR per trigger')
        judged = disagree = 0
        for path, meta in loaded:
            prefix = os.path.basename(path).split('_')[0]
            if not meta['camera_only'] or meta['mode'] != 0:
                raise Unusable(f'{prefix}: not a camera-only dump (camera_only '
                               f'{meta["camera_only"]}, mode {meta["mode"]}): merged motion '
                               'equals the object vectors where they exist')
            if not meta['object_valid']:
                print(f'{prefix}: no object vectors this frame: unjudged')
                continue
            proj_z = float(meta['proj_z'])
            if not math.isfinite(proj_z):
                raise Unusable(f'{prefix}: projection z scale {proj_z} is not finite')
            size = (int(meta['motion_size'][0]), int(meta['motion_size'][1]))
            height, width = size[1], size[0]
            camera = np.fromfile(companion(args.dump_dir, prefix, 'motion', 'rg16f', size, 4),
                                 np.float16).reshape(height, width, 2).astype(np.float32)
            obj = np.fromfile(companion(args.dump_dir, prefix, 'objects', 'rgba32f', size, 16),
                              np.float32).reshape(height, width, 4)
            depth = np.fromfile(companion(args.dump_dir, prefix, 'depth', 'f32', size, 4),
                                np.float32).reshape(height, width)
            threshold = np.maximum(2e-7, (proj_z - depth) * 0.01)
            finite = np.isfinite(threshold) & (threshold >= 0)
            valid = finite & (obj[..., 2] > 0.99) & (np.abs(obj[..., 3] - depth) < threshold) & \
                np.isfinite(obj[..., :2]).all(axis=2) & np.isfinite(camera).all(axis=2)
            count = int(valid.sum())
            if count < 1000:
                print(f'{prefix}: {count} comparable pixels (too few): unjudged')
                continue
            diff = np.abs(camera[valid] - obj[valid][:, :2])
            object_dy = obj[valid][:, 1]
            vertical = float(np.median(np.abs(object_dy)))
            med = np.median(diff, axis=0)
            p90 = np.percentile(diff, 90, axis=0)
            within = float(np.mean((diff < 0.25).all(axis=1)))
            moving = np.abs(object_dy) > 0.5
            sign_agree = float(np.mean(np.sign(camera[valid][:, 1][moving]) ==
                                       np.sign(object_dy[moving]))) if moving.any() else math.nan
            verdict = 'no vertical motion: unjudged'
            if vertical >= args.min_vertical:
                judged += 1
                ok = med[0] <= args.max_median and med[1] <= args.max_median
                disagree += 0 if ok else 1
                verdict = 'AGREE' if ok else 'DISAGREE'
            print(f'{prefix}: {count} px, object |dy| median {vertical:.2f}; |dx| median '
                  f'{med[0]:.3f} p90 {p90[0]:.3f}, |dy| median {med[1]:.3f} p90 {p90[1]:.3f}; '
                  f'within 0.25 px {within:.1%}; camera y sign = object y sign {sign_agree:.1%}: '
                  f'{verdict}')
    except Unusable as error:
        print(f'unusable: {error}')
        return 3
    except (KeyError, ValueError, TypeError, json.JSONDecodeError) as error:
        print(f'unusable: malformed meta or data: {error}')
        return 3
    if judged == 0:
        print('nothing judged (no object vectors, too few pixels or no vertical motion)')
        return 2
    print(f'{judged} frames judged, {disagree} disagree (over the selected pixels only)')
    return 1 if disagree else 0


if __name__ == '__main__':
    sys.exit(main())
