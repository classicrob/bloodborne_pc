import argparse
import contextlib
import importlib.util
import io
import json
import hashlib
from pathlib import Path
import tempfile
import unittest
from unittest import mock

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
        self.assertEqual((points, pixels), (None, [1920, 1080]))
        self.assertIn('object_motion=1\n', settings)
        settings, points, pixels = route.launch_display(self.args(native_1080p=False))
        self.assertEqual(settings, route.SETTINGS['1080p'])
        self.assertEqual((points, pixels), ((960, 540), None))

    def test_refuses_conflicting_preset_and_diagnostic_scaling(self):
        for changes in ({'output': '4k'}, {'ini': ['effect_dof=0']}, {'fps': '30'},
                        {'window_size': (960, 540)}, {'window_size': (1920, 1080)},
                        {'diag_env': ['BB_RENDER_RES=1280x720']},
                        {'diag_env': ['BB_PATCHES=NoFog']}, {'disable_optimizations': 1}):
            with self.subTest(changes=changes), self.assertRaises(SystemExit):
                route.launch_display(self.args(**changes))

    def test_generic_pixel_diagnostic_does_not_claim_native_qualification(self):
        args = self.args(native_1080p=False, diag_env=['BB_WINDOW_PIXELS=1920x1080'])
        settings, points, pixels = route.launch_display(args)
        self.assertEqual(settings, route.SETTINGS['1080p'])
        self.assertEqual((points, pixels), (None, None))
        args.window_size = (960, 540)
        with self.assertRaises(SystemExit):
            route.launch_display(args)

    def test_pixels_are_required_and_recorded_from_runtime(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp)
            (run / 'manifest.json').write_text(json.dumps({'display': {'expected_pixels': [1920, 1080]}}))
            for line in ('Window: 960x540 points (960x540 pixels)',
                         'GPU: window and Vulkan presenter ready'):
                (run / 'game.log').write_text(line + '\n')
                with self.assertRaises(ValueError):
                    route.report_window(run, wait=.1)
            (run / 'game.log').write_text('Window: 960x540 points (1920x1080 pixels)\n')
            with contextlib.redirect_stderr(io.StringIO()):
                route.report_window(run, wait=.1)
            event = json.loads((run / 'events.jsonl').read_text())
            self.assertEqual(event['pixels'], [1920, 1080])
            self.assertEqual(event['kind'], 'window-pixels-verified')

    def test_native_launch_refuses_missing_preview_before_artifact_or_pin_work(self):
        with mock.patch.object(route, 'verify_artifacts') as verify, \
                mock.patch.object(route, 'pin_artifacts') as pin, self.assertRaises(SystemExit):
            route.launch(self.args(preview_manifest=None))
        verify.assert_not_called()
        pin.assert_not_called()


class PreviewManifest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.artifacts = {}
        for name, relative in (('bb-probe', 'stage/bb-probe'), ('libbbgpu.dylib', 'stage/gpu/libbbgpu.dylib')):
            path = self.root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(name.encode())
            self.artifacts[name] = {'path': str(path), 'sha256': route.sha256(path)}
        self.value = {'qualification': {'test_gate': 'PASS'},
                      'source_tree': {'head': 'a' * 40, 'tree': 'b' * 40, 'tracked_diff_sha256': 'c' * 64},
                      'artifacts': self.artifacts}
        self.path = self.root / 'preview.json'

    def write(self):
        self.path.write_text(json.dumps(self.value))

    def test_pass_preserves_manifest_hash_and_full_source(self):
        self.write()
        result = route.verify_preview_manifest(self.path, self.artifacts, required=True)
        self.assertEqual(result['sha256'], hashlib.sha256(self.path.read_bytes()).hexdigest())
        self.assertEqual(result['source_tree'], self.value['source_tree'])
        self.assertEqual(result['test_gate'], 'PASS')

    def test_required_or_unexecuted_preview_cannot_qualify(self):
        with self.assertRaises(SystemExit): route.verify_preview_manifest(None, self.artifacts, required=True)
        self.assertIsNone(route.verify_preview_manifest(None, self.artifacts))
        for gate in (None, 'NOT_RUN', 'SKIPPED', 'FAILED'):
            self.value['qualification']['test_gate'] = gate
            self.write()
            with self.subTest(gate=gate), self.assertRaises(SystemExit):
                route.verify_preview_manifest(self.path, self.artifacts, required=True)

    def test_rejects_wrong_stage_pair_and_missing_source_head(self):
        self.write()
        for name in ('bb-probe', 'libbbgpu.dylib'):
            actual = {key: dict(value) for key, value in self.artifacts.items()}
            actual[name]['sha256'] = 'f' * 64
            with self.subTest(name=name), self.assertRaises(SystemExit):
                route.verify_preview_manifest(self.path, actual, required=True)
        copy = self.root / 'other-probe'; copy.write_bytes(b'bb-probe')
        actual = {key: dict(value) for key, value in self.artifacts.items()}
        actual['bb-probe']['path'] = str(copy)
        with self.assertRaises(SystemExit): route.verify_preview_manifest(self.path, actual, required=True)
        for head in (None, '', 'a' * 7):
            self.value['source_tree']['head'] = head
            self.write()
            with self.subTest(head=head), self.assertRaises(SystemExit):
                route.verify_preview_manifest(self.path, self.artifacts, required=True)


if __name__ == '__main__':
    unittest.main()
