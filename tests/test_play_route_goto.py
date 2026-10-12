"""play_route_goto: closed-loop walk-to-XYZ on the player-position file, against a simulated world with an
unknown camera heading and handedness, different speeds per real second (FPS), walls and bad samples."""
import importlib.util, json, math, os, tempfile, threading, time, unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location(
    'play_route_goto', Path(__file__).resolve().parents[1] / 'tools/macos/play_route_goto.py')
goto = importlib.util.module_from_spec(spec)
spec.loader.exec_module(goto)


class World:
    """A player that turns toward the stick at a limited rate before walking (as in the game), at
    `speed` units per real second; samples are published every 0.1 s by a background thread."""
    def __init__(self, path, cam, mirror, speed, wall_x=None, pid=42, run_id='a1', turn_rate=6.0):
        self.path, self.cam, self.mirror, self.speed, self.wall_x, self.pid = path, cam, mirror, speed, wall_x, pid
        self.run_id, self.started = run_id, goto.monotonic_ns() - 10**9
        self.turn_rate, self.facing = turn_rate, cam + 1.0
        self.x, self.z, self.seq, self.map, self.valid = 0.0, 0.0, 0, 0x15000000, True
        self.cmd, self.lock, self.running = None, threading.Lock(), False
        self.write()

    def write(self, sampled_ago_ns=1000, xyz=None):
        self.seq += 1
        sampled = goto.monotonic_ns() - sampled_ago_ns
        rec = {'version': 1, 'pid': self.pid, 'run_id': self.run_id, 'started_monotonic_ns': self.started,
               'sequence': self.seq, 'sample_monotonic_ns': sampled, 'published_monotonic_ns': sampled + 1000,
               'sample_age_ns': 1000, 'valid': self.valid, 'reason': 'ok' if self.valid else 'absent-player-chain'}
        if self.valid:
            rec.update(xyz=xyz or [self.x, 1.0, self.z], player_yaw_rad=self.facing, map_raw=self.map)
        with open(self.path + '.tmp', 'w') as f:
            f.write(json.dumps(rec))
        os.replace(self.path + '.tmp', self.path)

    def stick(self, lx, ly):
        with self.lock:
            self.cmd = None if lx is None else ((lx - 128) / 127, (128 - ly) / 127)

    def step(self, dt):
        with self.lock:
            cmd = self.cmd
        if cmd and math.hypot(*cmd) > 0.05:
            want = self.cam - self.mirror * math.atan2(cmd[0], cmd[1])
            diff = math.atan2(math.sin(want - self.facing), math.cos(want - self.facing))
            self.facing += max(-self.turn_rate * dt, min(self.turn_rate * dt, diff))
            if abs(diff) < 0.6:                      # walks once roughly facing the stick direction
                v = self.speed * min(1.0, math.hypot(*cmd)) * dt
                nx = self.x + math.cos(self.facing) * v
                if self.wall_x is None or nx < self.wall_x:
                    self.x = nx
                self.z += math.sin(self.facing) * v

    def __enter__(self):
        self.running = True
        def loop():
            while self.running:
                self.step(0.1); self.write(); time.sleep(0.1)
        self.thread = threading.Thread(target=loop, daemon=True); self.thread.start()
        return self

    def __exit__(self, *a):
        self.running = False; self.thread.join()


class GotoTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.path = os.path.join(self.tmp.name, 'pos.json')

    def tearDown(self):
        self.tmp.cleanup()

    def run_goto(self, world, target, **kw):
        with world:
            return goto.goto(goto.PositionReader(self.path, world.pid), world.stick, target, **kw)

    def test_reaches_target_for_any_camera_and_handedness_and_speed(self):
        # 12 combinations of camera heading, handedness and speed; the player turns before walking
        for cam in (0.3, 2.0, -2.5):
            for mirror in (1, -1):
                for speed in (2.0, 7.0):        # units per real second: low and high FPS
                    w = World(self.path, cam, mirror, speed)
                    r = self.run_goto(w, (9.0, -5.0), tol=0.8)
                    self.assertTrue(r['reached'], (cam, mirror, speed))
                    self.assertLessEqual(math.hypot(w.x - 9, w.z + 5), 1.2)   # plus one 0.1 s step after the read
                    self.assertIsNone(w.cmd, 'stick released')

    def test_wall_stops_loudly_and_releases(self):
        w = World(self.path, 0.0, 1, 4.0, wall_x=3.0)
        with self.assertRaisesRegex(goto.GotoError, 'no progress'):
            self.run_goto(w, (10.0, 0.0), tol=0.5)
        self.assertIsNone(w.cmd, 'stick released')

    def test_invalid_and_foreign_and_old_samples_stop(self):
        w = World(self.path, 0.0, 1, 4.0); w.valid = False; w.write()
        with self.assertRaisesRegex(goto.GotoError, 'invalid'):
            goto.goto(goto.PositionReader(self.path, w.pid), w.stick, (5.0, 0.0))
        w = World(self.path, 0.0, 1, 4.0, pid=7)
        with self.assertRaisesRegex(goto.GotoError, 'another process'):
            goto.goto(goto.PositionReader(self.path, 42), w.stick, (5.0, 0.0))
        w = World(self.path, 0.0, 1, 4.0); w.write(sampled_ago_ns=5 * 10**9)
        with self.assertRaisesRegex(goto.GotoError, 'old at read time'):
            goto.goto(goto.PositionReader(self.path, w.pid), w.stick, (5.0, 0.0))
        self.assertIsNone(w.cmd, 'stick released')

    def reader(self, w):
        return goto.PositionReader(self.path, w.pid, max_age_s=1.0)

    def test_old_first_record_is_not_used(self):
        # a stale file left from before: its sample_age_ns at publish is tiny, but it was sampled long ago
        w = World(self.path, 0.0, 1, 4.0); w.write(sampled_ago_ns=30 * 10**9)
        with self.assertRaisesRegex(goto.GotoError, 'old at read time'):
            self.reader(w).fresh(timeout_s=0.3)

    def test_newer_sequence_with_old_timestamp_is_not_used(self):
        w = World(self.path, 0.0, 1, 4.0); r = self.reader(w); r.fresh()
        w.write(sampled_ago_ns=3 * 10**9)
        with self.assertRaisesRegex(goto.GotoError, 'old at read time'):
            r.fresh(timeout_s=0.3)

    def test_run_id_change_at_same_pid_stops(self):
        w = World(self.path, 0.0, 1, 4.0); r = self.reader(w); r.fresh()
        w.run_id = 'b2'; w.write()
        with self.assertRaisesRegex(goto.GotoError, 'publisher changed'):
            r.fresh()

    def test_future_timestamp_stops(self):
        w = World(self.path, 0.0, 1, 4.0); w.write(sampled_ago_ns=-2 * 10**9)
        with self.assertRaisesRegex(goto.GotoError, 'in the future'):
            self.reader(w).fresh()

    def test_nonfinite_values_stop(self):
        w = World(self.path, 0.0, 1, 4.0); w.write(xyz=[float('nan'), 1.0, 0.0])
        with self.assertRaisesRegex(goto.GotoError, 'nonfinite'):
            self.reader(w).fresh()

    def test_stuck_sequence_stops(self):
        w = World(self.path, 0.0, 1, 4.0)       # no publisher thread: the sequence never advances
        with self.assertRaisesRegex(goto.GotoError, 'sequence stuck'):
            goto.goto(goto.PositionReader(self.path, w.pid), w.stick, (5.0, 0.0))
        self.assertIsNone(w.cmd, 'stick released')

    def test_map_change_stops(self):
        w = World(self.path, 0.0, 1, 4.0)
        reader = goto.PositionReader(self.path, w.pid)
        orig = reader.fresh
        def fresh(*a, **k):
            rec = orig(*a, **k)
            if reader.last_seq > 12:
                w.map += 1
            return rec
        reader.fresh = fresh
        with w, self.assertRaisesRegex(goto.GotoError, 'map changed'):
            goto.goto(reader, w.stick, (20.0, 0.0))
        self.assertIsNone(w.cmd, 'stick released')


if __name__ == '__main__':
    unittest.main()
