#!/usr/bin/env python3
"""Manual filesystem checks for save_fixture.py; no game data or game launch."""

import contextlib
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

import save_fixture as tool


class FixtureChecks(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / 'played'
        self.destination = self.root / 'edited'
        character = self.source / tool.CHARACTER
        character.parent.mkdir(parents=True)
        data = bytearray(tool.SOURCE_SIZE)
        data[4:8] = bytes.fromhex('00000118')
        data[0x118a8:0x118ac] = b'FACE'
        data[0x3ea49:0x3ea4d] = b'LCED'
        character.write_bytes(data)
        self.patch = mock.patch.object(tool, 'SOURCE_SHA256', tool.digest(data))
        self.patch.start()
        self.addCleanup(self.patch.stop)
        (self.source / 'fixture.json').write_text(json.dumps({'name': 'played', 'run': 'synthetic'}))
        (character.parent / 'backup0000').write_bytes(b'preserve backup')
        (character.parent / 'userdata0010').write_bytes(b'preserve global data')
        self.original = tool.inventory(self.source)

    def run_tool(self, dry=False):
        with contextlib.redirect_stdout(io.StringIO()) as output:
            result = tool.create(self.source, self.destination, dry)
        return result, output.getvalue()

    def test_dry_run(self):
        _, output = self.run_tool(True)
        self.assertIn('00000118 -> 00000015', output)
        self.assertIn('0x3ea5d', output)
        self.assertFalse(self.destination.exists())
        self.assertEqual(self.original, tool.inventory(self.source))

    def test_copy_edits_and_provenance(self):
        self.run_tool()
        self.assertEqual(self.original, tool.inventory(self.source))
        before = (self.source / tool.CHARACTER).read_bytes()
        after = (self.destination / tool.CHARACTER).read_bytes()
        self.assertEqual(after[4:8], bytes.fromhex('00000015'))
        self.assertEqual(after[0x3ea5d:0x3ea69], bytes.fromhex('000000c10000c0c0000090c1'))
        self.assertTrue(all(a == b or i in range(4, 8) or i in range(0x3ea5d, 0x3ea69)
                            for i, (a, b) in enumerate(zip(before, after))))
        self.assertEqual(len(before), len(after))
        for path in self.original:
            if path not in ('fixture.json', tool.CHARACTER.as_posix()):
                self.assertEqual((self.source / path).read_bytes(), (self.destination / path).read_bytes())
        meta = json.loads((self.destination / 'fixture.json').read_text())
        self.assertIs(meta['edited'], True)
        self.assertEqual(meta['validation'], 'pending')
        self.assertEqual(meta['edit_provenance']['source_files'], self.original)
        self.assertEqual(meta['edit_provenance']['source_fixture_sha256'],
                         tool.digest(tool.canonical(self.original)))
        self.assertNotEqual((self.source / tool.CHARACTER).stat().st_ino,
                            (self.destination / tool.CHARACTER).stat().st_ino)

    def test_existing_and_nested_refused(self):
        for destination in (self.source, self.source / 'child', self.root):
            with self.subTest(destination=destination), self.assertRaises(ValueError):
                tool.plan(self.source, destination)
        self.destination.mkdir()
        with self.assertRaises(ValueError):
            self.run_tool()
        self.assertTrue(self.destination.is_dir())
        self.assertEqual(self.original, tool.inventory(self.source))

    def test_unknown_source_refused(self):
        with (self.source / tool.CHARACTER).open('r+b') as file:
            file.write(b'changed')
        with self.assertRaisesRegex(ValueError, 'SHA256'):
            self.run_tool()
        self.assertFalse(self.destination.exists())

    def test_edited_source_refused(self):
        (self.source / 'fixture.json').write_text('{"edited": true}')
        with self.assertRaisesRegex(ValueError, 'ancestry'):
            self.run_tool()

    def test_symlink_refused(self):
        (self.source / 'alias').symlink_to(self.source / tool.CHARACTER)
        with self.assertRaisesRegex(ValueError, 'regular file'):
            self.run_tool()
        self.assertFalse(self.destination.exists())

    def test_failed_copy_removed(self):
        with mock.patch.object(tool.shutil, 'copytree', side_effect=OSError('injected copy failure')):
            with self.assertRaisesRegex(OSError, 'injected'):
                self.run_tool()
        self.assertFalse(self.destination.exists())
        self.assertEqual(self.original, tool.inventory(self.source))


if __name__ == '__main__':
    unittest.main()
