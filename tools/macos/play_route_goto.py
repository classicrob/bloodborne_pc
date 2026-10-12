"""Closed-loop "walk to XYZ" on Astra-2's player-position file (BB_PLAYER_POS_FILE, version 1).

The left stick moves the player relative to the CAMERA, whose heading the file does not carry. So the
right stick is never touched during a goto: the stick->world mapping stays fixed. It is learned from a
calibration push and refined from every observed step, so the result does not depend on frame rate.
Correct or loud: an invalid sample, a stale one (sequence not advancing, too old, another process), a
map change, no progress or a timeout stops with GotoError; it never steers on an old sample."""
import json, math, time


class GotoError(Exception):
    pass


def monotonic_ns():
    """The publisher's clock: CLOCK_MONOTONIC (system-wide on macOS, the same for the translated game)."""
    return time.clock_gettime_ns(time.CLOCK_MONOTONIC)


class PositionReader:
    """Reads the position file and returns only FRESH valid samples.

    Fresh means: a sequence newer than the last one used, from the same publisher (pid, run_id and
    started_monotonic_ns latched on the first record; a pid alone can be reused), sampled at most
    max_age_s before the READ on the publisher's clock (sample_age_ns is the age at publish and never
    grows in a stale file), not in the future, with finite coordinates. An invalid record, another
    publisher, a sequence going backwards, a future or nonfinite sample stop at once; a stale or old
    sample is waited past until timeout_s, then stops."""
    FUTURE_SLACK_NS = 50_000_000

    def __init__(self, path, pid, max_age_s=1.0, clock=time.monotonic, mono_ns=monotonic_ns):
        self.path, self.pid, self.max_age_s, self.clock, self.mono_ns = path, pid, max_age_s, clock, mono_ns
        self.last_seq = None
        self.identity = None

    def fresh(self, timeout_s=2.0):
        end = self.clock() + timeout_s
        why = 'no record'
        while True:
            try:
                with open(self.path) as f:
                    rec = json.loads(f.read())
            except (OSError, ValueError):
                rec = None
            if rec is not None:
                if rec.get('version') != 1 or rec.get('pid') != self.pid:
                    raise GotoError(f'position file is from another process or version: {rec.get("pid")}, v{rec.get("version")}')
                identity = (rec.get('pid'), rec.get('run_id'), rec.get('started_monotonic_ns'))
                if self.identity is None:
                    self.identity = identity
                elif identity != self.identity:
                    raise GotoError(f'position publisher changed: {identity} after {self.identity}')
                if not rec.get('valid'):
                    raise GotoError(f'position invalid: {rec.get("reason")}')
                seq = rec.get('sequence')
                if not isinstance(seq, int):
                    raise GotoError(f'position sequence is not an integer: {seq!r}')
                if self.last_seq is not None and seq < self.last_seq:
                    raise GotoError(f'position sequence went backwards: {seq} after {self.last_seq}')
                sampled = rec.get('sample_monotonic_ns')
                if not isinstance(sampled, int):
                    raise GotoError(f'position sample time is not an integer: {sampled!r}')
                age = self.mono_ns() - sampled
                if age < -self.FUTURE_SLACK_NS:
                    raise GotoError(f'position sample is {-age / 1e9:.3f} s in the future')
                values = list(rec.get('xyz') or []) + [rec.get('player_yaw_rad')]
                if len(values) != 4 or not all(isinstance(v, (int, float)) and math.isfinite(v) for v in values):
                    raise GotoError(f'position has nonfinite or missing values: {rec.get("xyz")}, {rec.get("player_yaw_rad")}')
                if self.last_seq is not None and seq == self.last_seq:
                    why = f'sequence stuck at {seq}'
                elif age > self.max_age_s * 1e9:
                    why = f'newest sample is {age / 1e9:.2f} s old at read time'
                else:
                    self.last_seq = seq
                    return rec
            if self.clock() > end:
                raise GotoError(f'no fresh position sample within {timeout_s} s ({why})')
            time.sleep(0.05)


def planar(rec):
    x, _, z = rec['xyz']
    return x, z


def _angle(d):
    return math.atan2(d[1], d[0])


def _wrap(a):
    return math.atan2(math.sin(a), math.cos(a))


def goto(reader, stick, target, tol=1.0, settle_s=0.6, measure_s=0.6, slice_s=0.35, timeout_s=120.0,
         stall_slices=8, clock=time.monotonic):
    """stick(lx, ly) holds the left stick (0..255, 128 neutral; ly=0 is forward) until the next call;
    stick(None, None) releases it. The player turns toward the stick before moving, so directions are
    measured only after settle_s of a continuous hold. target: (x, z) world units (xyz[0], xyz[2]).
    The stick is released on every exit path."""
    def measure(lx, ly, map0):
        stick(lx, ly)
        time.sleep(settle_s)
        a = reader.fresh()
        time.sleep(measure_s)
        b = reader.fresh()
        for rec in (a, b):
            if rec['map_raw'] != map0:
                raise GotoError('map changed during calibration')
        d = (planar(b)[0] - planar(a)[0], planar(b)[1] - planar(a)[1])
        if math.hypot(*d) < 0.05:
            raise GotoError('calibration push did not move the player (blocked, or input not reaching the game)')
        return _angle(d), b

    try:
        start = reader.fresh()
        map0 = start['map_raw']
        heading, _ = measure(128, 0, map0)            # world angle of stick-forward
        right, rec = measure(255, 128, map0)          # world angle of stick-right
        turn = _wrap(right - heading)
        if abs(abs(turn) - math.pi / 2) > math.pi / 4:
            raise GotoError(f'stick right moved {math.degrees(turn):.0f} deg from forward, not about 90 (camera moved?)')
        m = 1 if turn < 0 else -1
        pos = planar(rec)
        t0, best, stalled = clock(), math.dist(target, pos), 0
        while True:
            dist = math.dist(target, pos)
            if dist <= tol:
                return {'reached': True, 'distance': dist, 'xyz': rec['xyz'], 'sequence': rec['sequence']}
            if clock() - t0 > timeout_s:
                raise GotoError(f'timeout {timeout_s} s, {dist:.2f} units from the target')
            want = _angle((target[0] - pos[0], target[1] - pos[1]))
            rel = want - heading
            mag = 1.0 if dist > 3 * tol else 0.6       # slower near the target to avoid overshoot
            stick(round(128 + 127 * mag * math.sin(-m * rel)), round(128 - 127 * mag * math.cos(rel)))
            time.sleep(slice_s)
            rec = reader.fresh()
            if rec['map_raw'] != map0:
                raise GotoError('map changed during goto')
            nxt = planar(rec)
            step = (nxt[0] - pos[0], nxt[1] - pos[1])
            if math.hypot(*step) > 0.3:                # refine slowly: the turn lag biases short steps
                heading += 0.2 * _wrap(_angle(step) - want)
            pos = nxt
            d = math.dist(target, pos)
            if d < best - 0.05:
                best, stalled = d, 0
            else:
                stalled += 1
                if stalled >= stall_slices:
                    raise GotoError(f'no progress for {stall_slices} slices at {d:.2f} units (wall or obstacle)')
    finally:
        stick(None, None)
