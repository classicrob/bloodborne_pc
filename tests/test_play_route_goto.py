"""play_route_goto: closed-loop walk-to-XYZ on the player-position file, against a simulated world with an
unknown camera heading and handedness, different speeds per real second (FPS), walls and bad samples."""
import importlib.util, json, math, os, tempfile, unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location(
    'play_route_goto', Path(__file__).resolve().parents[1] / 'tools/macos/play_route_goto.py')
goto = importlib.util.module_from_spec(spec)
spec.loader.exec_module(goto)


class World:
    def __init__(self, path, cam, mirror, speed, wall_x=None, pid=42):
        self.path, self.cam, self.mirror, self.speed, self.wall_x, self.pid = path, cam, mirror, speed, wall_x, pid
        self.x, self.z, self.seq, self.map, self.valid = 0.0, 0.0, 0, 0x15000000, True
        self.write()

    def write(self, age_ns=1000):
        self.seq += 1
        rec = {'version': 1, 'pid': self.pid, 'sequence': self.seq, 'sample_age_ns': age_ns, 'valid': self.valid,
               'reason': None if self.valid else 'loading'}
        if self.valid:
            rec.update(xyz=[self.x, 1.0, self.z], player_yaw_rad=0.0, map_raw=self.map)
        with open(self.path + '.tmp', 'w') as f:
            f.write(json.dumps(rec))
        os.replace(self.path + '.tmp', self.path)

    def press(self, lx, ly, seconds):
        sx, sy = (lx - 128) / 127, (128 - ly) / 127
        mag = min(1.0, math.hypot(sx, sy))
        if mag > 0.05:
            ang = self.cam - self.mirror * math.atan2(sx, sy)
            nx = self.x + math.cos(ang) * self.speed * seconds * mag
            if self.wall_x is None or nx < self.wall_x:
                self.x = nx
            self.z += math.sin(ang) * self.speed * seconds * mag
        self.write()


class GotoTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.path = os.path.join(self.tmp.name, 'pos.json')

    def tearDown(self):
        self.tmp.cleanup()

    def run_goto(self, world, target, **kw):
        return goto.goto(goto.PositionReader(self.path, world.pid), world.press, target, **kw)

    def test_reaches_target_for_any_camera_and_handedness_and_speed(self):
        for cam in (0.3, 2.0, -2.5):
            for mirror in (1, -1):
                for speed in (2.0, 7.0):        # units per real second: low and high FPS
                    w = World(self.path, cam, mirror, speed)
                    r = self.run_goto(w, (12.0, -7.0), tol=0.8)
                    self.assertTrue(r['reached'], (cam, mirror, speed))
                    self.assertLessEqual(math.hypot(w.x - 12, w.z + 7), 0.8)

    def test_wall_stops_loudly(self):
        w = World(self.path, 0.0, 1, 4.0, wall_x=3.0)
        with self.assertRaisesRegex(goto.GotoError, 'no progress'):
            self.run_goto(w, (10.0, 0.0), tol=0.5)

    def test_invalid_and_foreign_and_old_samples_stop(self):
        w = World(self.path, 0.0, 1, 4.0); w.valid = False; w.write()
        with self.assertRaisesRegex(goto.GotoError, 'invalid'):
            self.run_goto(w, (5.0, 0.0))
        w = World(self.path, 0.0, 1, 4.0, pid=7)
        with self.assertRaisesRegex(goto.GotoError, 'another process'):
            goto.goto(goto.PositionReader(self.path, 42), w.press, (5.0, 0.0))
        w = World(self.path, 0.0, 1, 4.0); w.write(age_ns=5e9)
        with self.assertRaisesRegex(goto.GotoError, 'old'):
            self.run_goto(w, (5.0, 0.0))

    def test_stuck_sequence_stops(self):
        w = World(self.path, 0.0, 1, 4.0)
        frozen = lambda lx, ly, s: None          # input never reaches the game; no new samples
        with self.assertRaisesRegex(goto.GotoError, 'no new position sample'):
            goto.goto(goto.PositionReader(self.path, w.pid), frozen, (5.0, 0.0))

    def test_map_change_stops(self):
        w = World(self.path, 0.0, 1, 4.0)
        orig = w.press
        def press(lx, ly, s):
            orig(lx, ly, s)
            if w.seq > 4: w.map += 1; w.write()
        with self.assertRaisesRegex(goto.GotoError, 'map changed'):
            goto.goto(goto.PositionReader(self.path, w.pid), press, (20.0, 0.0))


if __name__ == '__main__':
    unittest.main()
