import argparse
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('play_route_display', Path(__file__).resolve().parents[1] /
                                            'tools/macos/play_route.py')
route = importlib.util.module_from_spec(spec)
spec.loader.exec_module(route)


class NativeDisplay(unittest.TestCase):
    def args(self, **changes):
        values = dict(output='1080p', ini=[], fps='60', native_1080p=True,
                      window_size=None, diag_env=[], disable_optimizations=None)
        values.update(changes)
        return argparse.Namespace(**values)

    def test_native_mode_and_existing_default_are_distinct(self):
        settings, points, pixels = route.launch_display(self.args())
        self.assertEqual((points, pixels), ((1920, 1080), [1920, 1080]))
        self.assertIn('object_motion=1\n', settings)
        settings, points, pixels = route.launch_display(self.args(native_1080p=False))
        self.assertEqual(settings, route.SETTINGS['1080p'])
        self.assertEqual((points, pixels), ((960, 540), None))

    def test_refuses_conflicting_preset_and_diagnostic_scaling(self):
        for changes in ({'output': '4k'}, {'ini': ['effect_dof=0']}, {'fps': '30'},
                        {'window_size': (960, 540)}, {'diag_env': ['BB_RENDER_RES=1280x720']},
                        {'diag_env': ['BB_PATCHES=NoFog']}, {'disable_optimizations': 1}):
            with self.subTest(changes=changes), self.assertRaises(SystemExit):
                route.launch_display(self.args(**changes))

    def test_pixels_are_required_and_recorded_from_runtime(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp)
            (run / 'manifest.json').write_text(json.dumps({'display': {'expected_pixels': [1920, 1080]}}))
            for line in ('Window: 960x540 points (960x540 pixels)',
                         'GPU: window and Vulkan presenter ready'):
                (run / 'game.log').write_text(line + '\n')
                with self.assertRaises(ValueError):
                    route.report_window(run, wait=.1)
            (run / 'game.log').write_text('Window: 1920x1080 points (1920x1080 pixels)\n')
            with contextlib.redirect_stderr(io.StringIO()):
                route.report_window(run, wait=.1)
            event = json.loads((run / 'events.jsonl').read_text())
            self.assertEqual(event['pixels'], [1920, 1080])
            self.assertEqual(event['kind'], 'window-pixels-verified')


if __name__ == '__main__':
    unittest.main()
