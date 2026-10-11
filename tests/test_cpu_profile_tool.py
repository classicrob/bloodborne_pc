import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('cpu_profile', Path(__file__).parents[1] / 'tools/macos/cpu_profile.py')
profile = importlib.util.module_from_spec(spec)
spec.loader.exec_module(profile)


class ProfileSymbols(unittest.TestCase):
    def test_guest_and_prx_priority(self):
        symbols = profile.Symbols([
            {'type': 'guest', 'module': -1, 'base': 0x8000, 'size': 0x4000},
            {'type': 'guest', 'module': 0, 'base': 0x9000, 'size': 0x1000},
        ], symbolize=False)
        self.assertEqual(symbols.describe(0x8123, 5), ('eboot+0x123', 'guest'))
        self.assertEqual(symbols.describe(0x9123, 5), ('prx[0]+0x123', 'guest'))

    def test_unloaded_image_and_address_reuse(self):
        rows = [
            {'type': 'image', 'base': 0x1000, 'end': 0x2000, 'ticks': 1, 'path': '/a/libbbgpu.dylib', 'uuid': 'a'},
            {'type': 'image-remove', 'base': 0x1000, 'ticks': 4},
            {'type': 'image', 'base': 0x1000, 'end': 0x2000, 'ticks': 6, 'path': '/b/libMoltenVK.dylib', 'uuid': 'b'},
        ]
        symbols = profile.Symbols(rows, symbolize=False)
        self.assertEqual(symbols.describe(0x1234, 2), ('libbbgpu.dylib+0x234', 'GPU-library'))
        self.assertEqual(symbols.describe(0x1234, 5), ('unknown+0x1234', 'other'))
        self.assertEqual(symbols.describe(0x1234, 7), ('libMoltenVK.dylib+0x234', 'MoltenVK'))

    def test_mismatched_image_not_symbolized(self):
        from unittest.mock import patch
        from subprocess import CompletedProcess
        with tempfile.NamedTemporaryFile() as f:
            symbols = profile.Symbols([{'type': 'image', 'base': 1, 'end': 100, 'ticks': 1,
                                        'path': f.name, 'uuid': 'abcd'}])
            with patch.object(profile.subprocess, 'run', return_value=CompletedProcess([], 0, 'UUID: 1234 (x86_64)\n')) as call:
                symbols.prepare([{'pc': [2], 'ticks': 2}])
                self.assertEqual(call.call_count, 1)
            self.assertFalse(symbols.cache)
            self.assertTrue(any('UUID mismatch' in warning for warning in symbols.warnings))

    def test_truncated_final_record_only(self):
        with tempfile.TemporaryDirectory() as directory:
            p = Path(directory)/'capture'
            p.write_text('{"type":"config","version":1}\n{"type":')
            rows, _ = profile.read_capture(p)
            self.assertEqual(len(rows), 1)
            p.write_text('{"type":"config","version":1}\nbad\n{}\n')
            with self.assertRaises(ValueError):
                profile.read_capture(p)


if __name__ == '__main__':
    unittest.main()
