#!/usr/bin/env python3
"""Camera motion vectors against the GPU's own object vectors, from an upscaler dump.

Run the game with BB_MOTION_CAMERA_ONLY=1 BB_DUMP_TRIGGER=<file> BB_DUMP_DIR=<dir>
(BB_DUMP_FRAMES=8 by default) and an upscaler on, create <file> while the camera pitches or moves
vertically (stairs), then:

    python3 tools/macos/motion_agree.py <dir> [--max-median 0.1]

Per dumped frame it compares fNNN_motion (camera-only, rg16f) with fNNN_objects (rgba32f: object
vector rg, valid b, depth a) on pixels where the object vector is valid and its depth equals the
frame's depth (fNNN_depth, f32): the surfaces the object pass drew last. On static meshes the two
must agree; moving objects (characters, cloth) legitimately differ, so the verdict uses medians.
Prints per frame: pixels compared, vertical object motion, median and p90 |dx| and |dy|, the
fraction within 0.25 px, and whether the camera y agrees in sign with the object y. Exit 0 when
every frame with enough vertical motion agrees (median |dx| and |dy| <= --max-median), 1 when one
does not, 2 when there is nothing to judge (no valid object pixels or no vertical motion).
"""
import argparse
import glob
import os
import re
import sys

import numpy as np


def load(path, dtype, channels):
    match = re.search(r'_(\d+)x(\d+)_', os.path.basename(path))
    width, height = int(match.group(1)), int(match.group(2))
    data = np.fromfile(path, dtype=dtype)
    return data.reshape(height, width, channels) if channels > 1 else data.reshape(height, width)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('dump_dir')
    parser.add_argument('--max-median', type=float, default=0.1,
                        help='largest median |camera - object| in px that counts as agreement')
    parser.add_argument('--min-vertical', type=float, default=0.5,
                        help='median |object dy| in px a frame needs to judge the y convention')
    args = parser.parse_args()

    frames = sorted(glob.glob(os.path.join(args.dump_dir, 'f*_motion_*_rg16f.raw')))
    if not frames:
        print(f'no fNNN_motion_*_rg16f.raw in {args.dump_dir}')
        return 2
    judged = disagree = 0
    for motion_path in frames:
        prefix = os.path.basename(motion_path).split('_')[0]
        objects = glob.glob(os.path.join(args.dump_dir, f'{prefix}_objects_*_rgba32f.raw'))
        depths = glob.glob(os.path.join(args.dump_dir, f'{prefix}_depth_*_f32.raw'))
        if not objects or not depths:
            print(f'{prefix}: no objects or depth dump (object motion off or another size)')
            continue
        camera = load(motion_path, np.float16, 2).astype(np.float32)
        obj = load(objects[0], np.float32, 4)
        depth = load(depths[0], np.float32, 1)
        if camera.shape[:2] != obj.shape[:2] or depth.shape != obj.shape[:2]:
            print(f'{prefix}: sizes differ (motion {camera.shape}, objects {obj.shape}, '
                  f'depth {depth.shape})')
            continue
        threshold = np.maximum(2e-7, (1.0 - depth) * 0.01)
        valid = (obj[..., 2] > 0.99) & (np.abs(obj[..., 3] - depth) < threshold) & \
            np.isfinite(obj[..., 0]) & np.isfinite(obj[..., 1]) & np.isfinite(camera).all(axis=2)
        count = int(valid.sum())
        if count < 1000:
            print(f'{prefix}: {count} comparable pixels (too few)')
            continue
        diff = np.abs(camera[valid] - obj[valid][:, :2])
        object_dy = obj[valid][:, 1]
        vertical = float(np.median(np.abs(object_dy)))
        med = np.median(diff, axis=0)
        p90 = np.percentile(diff, 90, axis=0)
        within = float(np.mean((diff < 0.25).all(axis=1)))
        moving = np.abs(object_dy) > 0.5
        sign_agree = float(np.mean(np.sign(camera[valid][:, 1][moving]) == np.sign(object_dy[moving]))) \
            if moving.any() else float('nan')
        verdict = 'no vertical motion'
        if vertical >= args.min_vertical:
            judged += 1
            ok = med[0] <= args.max_median and med[1] <= args.max_median
            disagree += 0 if ok else 1
            verdict = 'AGREE' if ok else 'DISAGREE'
        print(f'{prefix}: {count} px, object |dy| median {vertical:.2f}; |dx| median {med[0]:.3f} '
              f'p90 {p90[0]:.3f}, |dy| median {med[1]:.3f} p90 {p90[1]:.3f}; within 0.25 px '
              f'{within:.1%}; camera y sign = object y sign {sign_agree:.1%}: {verdict}')
    if judged == 0:
        print('nothing judged: no frame with enough vertical object motion')
        return 2
    print(f'{judged} frames judged, {disagree} disagree')
    return 1 if disagree else 0


if __name__ == '__main__':
    sys.exit(main())
