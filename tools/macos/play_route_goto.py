"""Closed-loop "walk to XYZ" on Astra-2's player-position file (BB_PLAYER_POS_FILE, version 1).

The left stick moves the player relative to the CAMERA, whose heading the file does not carry. So the
right stick is never touched during a goto: the stick->world mapping stays fixed. It is learned from a
calibration push and refined from every observed step, so the result does not depend on frame rate.
Correct or loud: an invalid sample, a stale one (sequence not advancing, too old, another process), a
map change, no progress or a timeout stops with GotoError; it never steers on an old sample."""
import json, math, time


class GotoError(Exception):
    pass


class PositionReader:
    def __init__(self, path, pid, max_age_s=1.0, clock=time.monotonic):
        self.path, self.pid, self.max_age_s, self.clock = path, pid, max_age_s, clock
        self.last_seq = None

    def fresh(self, timeout_s=2.0):
        """Next valid sample with a sequence newer than the last one used, else GotoError."""
        end = self.clock() + timeout_s
        while True:
            try:
                with open(self.path) as f:
                    rec = json.loads(f.read())
            except (OSError, ValueError):
                rec = None
            if rec is not None:
                if rec.get('version') != 1 or rec.get('pid') != self.pid:
                    raise GotoError(f'position file is from another process or version: {rec.get("pid")}, v{rec.get("version")}')
                if not rec.get('valid'):
                    raise GotoError(f'position invalid: {rec.get("reason")}')
                if rec['sample_age_ns'] > self.max_age_s * 1e9:
                    raise GotoError(f'position sample is {rec["sample_age_ns"] / 1e9:.2f} s old')
                if self.last_seq is None or rec['sequence'] > self.last_seq:
                    self.last_seq = rec['sequence']
                    return rec
            if self.clock() > end:
                raise GotoError(f'no new position sample within {timeout_s} s (sequence stuck at {self.last_seq})')
            time.sleep(0.05)


def planar(rec):
    x, _, z = rec['xyz']
    return x, z


def goto(reader, press, target, tol=1.0, slice_s=0.4, timeout_s=120.0, stall_slices=6, clock=time.monotonic):
    """press(lx, ly, seconds) holds the left stick (0..255, 128 neutral; ly=0 is forward).
    target: (x, z) in world units (the file's xyz[0], xyz[2]); y is ignored."""
    start = reader.fresh()
    map0 = start['map_raw']
    pos = planar(start)
    # calibration: world direction of stick "forward"
    press(128, 0, slice_s)
    rec = reader.fresh()
    if rec['map_raw'] != map0:
        raise GotoError('map changed during calibration')
    nxt = planar(rec)
    d = (nxt[0] - pos[0], nxt[1] - pos[1])
    if math.hypot(*d) < 1e-3:
        raise GotoError('calibration push did not move the player (blocked or input not reaching the game)')
    heading = math.atan2(d[1], d[0])     # world angle of stick-forward
    pos = nxt
    # handedness: is stick-right clockwise (m=+1) or counter-clockwise (m=-1) from forward in world angles?
    press(255, 128, slice_s)
    rec = reader.fresh()
    if rec['map_raw'] != map0:
        raise GotoError('map changed during calibration')
    nxt = planar(rec)
    r = (nxt[0] - pos[0], nxt[1] - pos[1])
    if math.hypot(*r) < 1e-3:
        raise GotoError('calibration push right did not move the player')
    turn = math.atan2(math.sin(math.atan2(r[1], r[0]) - heading), math.cos(math.atan2(r[1], r[0]) - heading))
    if abs(abs(turn) - math.pi / 2) > math.pi / 4:
        raise GotoError(f'stick right moved {math.degrees(turn):.0f} deg from forward, not about 90 (camera moved?)')
    m = 1 if turn < 0 else -1
    pos = nxt
    t0, best, stalled = clock(), math.hypot(target[0] - pos[0], target[1] - pos[1]), 0
    while True:
        dist = math.hypot(target[0] - pos[0], target[1] - pos[1])
        if dist <= tol:
            return {'reached': True, 'distance': dist, 'xyz': rec['xyz'], 'sequence': rec['sequence']}
        if clock() - t0 > timeout_s:
            raise GotoError(f'timeout {timeout_s} s, {dist:.2f} units from the target')
        want = math.atan2(target[1] - pos[1], target[0] - pos[0])
        rel = want - heading                      # angle from stick-forward to the target, world frame
        lx = 128 + 127 * math.sin(-m * rel)       # stick (sx right, sy forward) at world angle heading - m*atan2(sx, sy)
        ly = 128 - 127 * math.cos(rel)
        press(round(max(0, min(255, lx))), round(max(0, min(255, ly))), min(slice_s, max(0.1, dist * 0.1)))
        rec = reader.fresh()
        if rec['map_raw'] != map0:
            raise GotoError('map changed during goto')
        nxt = planar(rec)
        step = (nxt[0] - pos[0], nxt[1] - pos[1])
        if math.hypot(*step) > 1e-3:              # refine: observed direction vs the commanded one
            heading += math.atan2(math.sin(math.atan2(step[1], step[0]) - want), math.cos(math.atan2(step[1], step[0]) - want)) * 0.5
        pos = nxt
        d = math.hypot(target[0] - pos[0], target[1] - pos[1])
        if d < best - 0.05:
            best, stalled = d, 0
        else:
            stalled += 1
            if stalled >= stall_slices:
                raise GotoError(f'no progress for {stall_slices} slices at {d:.2f} units (wall or obstacle)')
