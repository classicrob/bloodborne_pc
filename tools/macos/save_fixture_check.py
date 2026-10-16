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




class WeaponFixtureChecks(unittest.TestCase):
    def setUp(self):
        import struct
        import save_weapon_fixture as weapon
        self.weapon = weapon
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / 'dream-exited'
        self.destination = self.root / 'weapons'
        character = self.source / tool.CHARACTER
        character.parent.mkdir(parents=True)
        data = bytearray(b'Z' * tool.SOURCE_SIZE)
        data[:0x54] = bytes(0x54)
        struct.pack_into('<I', data, 0, 0x41)
        pos = 0x54
        for i in range(4096):
            if i < 5 or 71 <= i <= 79:
                handle = 0x90800000 | (i if i < 5 else i)
                record = struct.pack('<15I', handle, 0x10000000 + i, *([0] * 13))
            else:
                record = struct.pack('<2I', 0, 0xffffffff)
            data[pos:pos + len(record)] = record
            pos += len(record)
        self.assertEqual(pos, 0x832c)
        data[0x10a84:0x10a88] = b'FACE'
        for i in range(1984):
            off = 0x858c + 16*i
            struct.pack_into('<4I', data, off, i+1 if i < 10 else 0,
                             100+i if i < 10 else 0xffffffff, 1 if i < 10 else 0, i)
        struct.pack_into('<I', data, 0x8588, 10)
        struct.pack_into('<I', data, 0x10994, 75)
        struct.pack_into('<2I', data, 0x4c, 0x3dbb0, 0x20004)
        struct.pack_into('<2I', data, 0x5dbb4, 1, 4096)
        character.write_bytes(data)
        (self.source / 'fixture.json').write_text(json.dumps({
            'name': 'dream-exited', 'edited': True, 'edited_origin': 'dream-location',
            'parent': {'name': 'dream-location', 'edited': True, 'edits': ['position']}}))
        (character.parent / 'userdata0010').write_bytes(b'untouched global state')
        self.original = tool.inventory(self.source)
        patcher = mock.patch.object(weapon, 'SOURCE_SHA256', tool.digest(data))
        patcher.start()
        self.addCleanup(patcher.stop)

    def run_tool(self, dry=False):
        with contextlib.redirect_stdout(io.StringIO()) as output:
            result = self.weapon.create(self.source, self.destination, dry)
        return result, output.getvalue()

    def test_weapon_dry_run(self):
        _, output = self.run_tool(True)
        self.assertIn('BYTE DIFF', output)
        self.assertIn('DRY RUN', output)
        self.assertFalse(self.destination.exists())
        self.assertEqual(tool.inventory(self.source), self.original)

    def test_weapon_copy_roundtrip_tail_and_ancestry(self):
        result, _ = self.run_tool()
        before = (self.source / tool.CHARACTER).read_bytes()
        after = (self.destination / tool.CHARACTER).read_bytes()
        self.assertEqual(self.weapon.reverse(after, result['edit']), before)
        self.assertEqual(len(before), len(after))
        self.assertEqual(before[0x5ec3f:], after[0x5ec3f:])
        self.assertEqual(tool.inventory(self.source), self.original)
        parent = json.loads((self.source / 'fixture.json').read_text())
        meta = json.loads((self.destination / 'fixture.json').read_text())
        self.assertEqual(meta['parent'], parent)
        self.assertTrue(meta['edited'])
        self.assertEqual(meta['validation'], 'pending')
        self.assertEqual(meta['edit_provenance']['source_metadata'], parent)

    def test_weapon_hash_refusal(self):
        path = self.source / tool.CHARACTER
        data = bytearray(path.read_bytes()); data[-1] ^= 1; path.write_bytes(data)
        with self.assertRaisesRegex(ValueError, 'SHA256'):
            self.run_tool()
        self.assertFalse(self.destination.exists())

    def test_weapon_existing_or_nested_refusal(self):
        for destination in (self.source, self.source/'child', self.root):
            with self.assertRaises(ValueError):
                self.weapon.create(self.source, destination)
        self.destination.mkdir()
        with self.assertRaises(ValueError):
            self.run_tool()

    def test_weapon_copy_failure_cleanup(self):
        with mock.patch.object(self.weapon.shutil, 'copytree', side_effect=OSError('injected')):
            with self.assertRaises(OSError):
                self.run_tool()
        self.assertFalse(self.destination.exists())
        self.assertEqual(tool.inventory(self.source), self.original)

    def test_weapon_reverse_rejects_modified_edit(self):
        data = (self.source / tool.CHARACTER).read_bytes()
        edited, operation = self.weapon.edit_character(data)
        changed = bytearray(edited); changed[0x10] ^= 1
        with self.assertRaises(ValueError):
            self.weapon.reverse(changed, operation)




class CentralFixtureChecks(unittest.TestCase):
    def setUp(self):
        import struct
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / 'clinic-via-sickroom-lamp'
        self.destination = self.root / 'central'
        character = self.source / tool.CHARACTER
        character.parent.mkdir(parents=True)
        data = bytearray(b'Z' * tool.SOURCE_SIZE)
        data[4:8] = struct.pack('<I', 0x18010000)
        data[0x12548:0x1254c] = b'FACE'
        data[0x3f7a1:0x3f7a5] = b'LCED'
        data[0x3f7a9:0x3f7b5] = bytes.fromhex('ffffffff0000000000000000')
        data[0x3f7b5:0x3f7c1] = struct.pack('<3f', -198.958, -50.64, 160.58)
        character.write_bytes(data)
        self.meta = {'name': self.source.name, 'run': 'synthetic', 'edited': True,
                     'edited_origin': 'dream-location',
                     'parent': {'name': 'weapons', 'edited': True},
                     'unverified': ['lamps'],
                     'verified': [{'item': 'pistol-fire', 'fixture': 'weapons', 'run': 'old'}]}
        (self.source / 'fixture.json').write_text(json.dumps(self.meta))
        (character.parent / 'backup0000').write_bytes(b'unchanged backup')
        self.original = tool.inventory(self.source)
        patcher = mock.patch.object(tool, 'CENTRAL_SOURCE_SHA256', tool.digest(data))
        patcher.start(); self.addCleanup(patcher.stop)

    def run_tool(self, dry=False):
        with contextlib.redirect_stdout(io.StringIO()) as output:
            result = tool.create(self.source, self.destination, dry, 'central-yharnam')
        return result, output.getvalue()

    def test_central_exact_position_and_reverse(self):
        import struct
        provenance, _ = self.run_tool()
        before = (self.source / tool.CHARACTER).read_bytes()
        after = (self.destination / tool.CHARACTER).read_bytes()
        self.assertEqual(len(after), len(before))
        self.assertEqual(after[:0x3f7b5], before[:0x3f7b5])
        self.assertEqual(after[0x3f7c1:], before[0x3f7c1:])
        self.assertEqual(after[0x3f7b5:0x3f7c1], struct.pack('<3f', -193.4, -28.646, 68.5))
        self.assertEqual(tool.reverse(after, provenance['edits']), before)
        self.assertEqual(tool.inventory(self.source), self.original)
        meta = json.loads((self.destination / 'fixture.json').read_text())
        self.assertEqual(meta['parent'], self.meta)
        self.assertEqual(meta['verified'], self.meta['verified'])
        self.assertTrue(meta['edited'])
        self.assertEqual(meta['edited_origin'], 'dream-location')
        self.assertEqual(meta['validation'], 'pending')

    def test_central_dry_run(self):
        _, output = self.run_tool(True)
        self.assertIn('0x3f7b5', output)
        self.assertIn('DRY RUN', output)
        self.assertFalse(self.destination.exists())
        self.assertEqual(tool.inventory(self.source), self.original)

    def test_central_wrong_hash_or_profile(self):
        with self.assertRaises(ValueError):
            tool.plan(self.source, self.destination)
        with self.assertRaises(ValueError):
            tool.plan(self.source, self.destination, 'unknown')
        path = self.source / tool.CHARACTER
        data = bytearray(path.read_bytes()); data[-1] ^= 1; path.write_bytes(data)
        with self.assertRaisesRegex(ValueError, 'SHA256'):
            self.run_tool()

    def test_central_guards_and_tampering(self):
        path = self.source / tool.CHARACTER
        data = bytearray(path.read_bytes()); data[4] ^= 1; path.write_bytes(data)
        with mock.patch.object(tool, 'CENTRAL_SOURCE_SHA256', tool.digest(data)):
            with self.assertRaisesRegex(ValueError, 'map/coordinate'):
                self.run_tool()
        data[4] ^= 1; path.write_bytes(data)
        provenance, _ = self.run_tool(True)
        with self.assertRaisesRegex(ValueError, 'no longer matches'):
            tool.reverse(data, provenance['edits'])


class CombatFixtureChecks(unittest.TestCase):
    def setUp(self):
        import struct
        import save_combat_fixture as combat
        self.combat = combat
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        root = Path(self.temp.name)
        self.source, self.destination = root / combat.SOURCE_NAME, root / 'combat'
        character = self.source / tool.CHARACTER
        character.parent.mkdir(parents=True)
        data = bytearray(b'Z' * tool.SOURCE_SIZE)
        data[:0x54] = bytes(0x54)
        struct.pack_into('<2I', data, 0, 0x41, 0x18010000)
        pos = 0x54
        for i in range(4096):
            size, kind = (60, 0x80) if i < 139 else ((40, 0xc0) if i < 150 else (8, 0))
            data[pos:pos + size] = bytes(size)
            data[pos + 3] = kind
            pos += size
        self.assertEqual(pos, 0x9df0)
        data[0x12548:0x1254c] = b'FACE'
        for i in range(1984):
            struct.pack_into('<4I', data, 0xa050 + i*16,
                             i+1 if i < 13 else 0, 100+i if i < 13 else 0xffffffff,
                             1 if i < 13 else 0, i)
        for _, offset, before, _ in combat.FIELDS:
            struct.pack_into('<I', data, offset, before)
        struct.pack_into('<2I', data, 0x4c, 0x3f805, 0x20004)
        struct.pack_into('<2I', data, 0x5f809, 1, 4096)
        character.write_bytes(data)
        self.parent = {'name': combat.SOURCE_NAME, 'edited': True,
                       'edited_origin': 'hunters-dream-edited',
                       'parent': {'name': 'played'}, 'unverified': ['flags'],
                       'unverified_history': [{'items': ['load'], 'reason': 'previous run'}],
                       'verified': [{'item': 'load', 'fixture': 'ancestor', 'run': 'old-run'}]}
        (self.source / 'fixture.json').write_text(json.dumps(self.parent))
        (character.parent / 'backup0000').write_bytes(b'backup unchanged')
        self.original = tool.inventory(self.source)
        self.data = bytes(data)
        patcher = mock.patch.object(combat, 'SOURCE_SHA256', tool.digest(data))
        patcher.start()
        self.addCleanup(patcher.stop)

    def run_tool(self, dry=False):
        with contextlib.redirect_stdout(io.StringIO()) as output:
            result = self.combat.create(self.source, self.destination, dry)
        return result, output.getvalue()

    def test_combat_dry_run(self):
        _, output = self.run_tool(True)
        self.assertIn('blood_vials_20 @0xa120', output)
        self.assertIn('DRY RUN', output)
        self.assertFalse(self.destination.exists())
        self.assertEqual(self.original, tool.inventory(self.source))

    def test_combat_roundtrip_bounded_changes_and_history(self):
        p, _ = self.run_tool()
        after = (self.destination / tool.CHARACTER).read_bytes()
        self.assertEqual(tool.reverse(after, p['edits']), self.data)
        allowed = set()
        for c in p['edits']:
            allowed.update(range(c['offset'], c['offset'] + len(bytes.fromhex(c['after_hex']))))
        self.assertTrue(all(a == b or i in allowed for i, (a, b) in enumerate(zip(self.data, after))))
        self.assertEqual(after[0xa12c:0xa130], self.data[0xa12c:0xa130])
        self.assertEqual(self.original, tool.inventory(self.source))
        self.assertNotEqual((self.source/tool.CHARACTER).stat().st_ino,
                            (self.destination/tool.CHARACTER).stat().st_ino)
        metadata = json.loads((self.destination/'fixture.json').read_text())
        self.assertEqual(metadata['parent'], self.parent)
        for k in ('verified', 'unverified_history', 'edited_origin'):
            self.assertEqual(metadata[k], self.parent[k])
        self.assertEqual(metadata['unverified'], ['flags'] + self.combat.PENDING)
        self.assertEqual(metadata['validation'], 'pending')
        self.assertTrue(metadata['edited'])

    def test_combat_hash_and_ancestry_refusal(self):
        with mock.patch.object(self.combat, 'SOURCE_SHA256', '0'*64):
            with self.assertRaisesRegex(ValueError, 'SHA256'):
                self.run_tool()
        (self.source/'fixture.json').write_text('{"name":"wrong", "edited":true}')
        with self.assertRaisesRegex(ValueError, 'ancestry'):
            self.run_tool()
        self.assertFalse(self.destination.exists())

    def test_combat_rejects_occupied_slot_and_bad_stat_after_hash_gate(self):
        for offset, reason in ((0xa120, 'vacant'), (0x9e70, 'level')):
            changed = bytearray(self.data); changed[offset] ^= 1
            with mock.patch.object(self.combat, 'SOURCE_SHA256', tool.digest(changed)):
                with self.assertRaisesRegex(ValueError, reason):
                    self.combat.edit_character(changed)

    def test_combat_existing_nested_and_symlink_refusal(self):
        for dest in (self.source, self.source/'child', self.source.parent):
            with self.assertRaises(ValueError):
                self.combat.create(self.source, dest)
        (self.source/'link').symlink_to(self.source/tool.CHARACTER)
        with self.assertRaisesRegex(ValueError, 'regular file'):
            self.run_tool()

    def test_combat_failed_copy_cleanup(self):
        with mock.patch.object(self.combat.shutil, 'copytree', side_effect=OSError('injected')):
            with self.assertRaises(OSError):
                self.run_tool()
        self.assertFalse(self.destination.exists())
        self.assertEqual(self.original, tool.inventory(self.source))

    def test_combat_reverse_rejects_tamper(self):
        edited, edits = self.combat.edit_character(self.data)
        changed = bytearray(edited); changed[0x9e20] ^= 1
        with self.assertRaisesRegex(ValueError, 'no longer matches'):
            tool.reverse(changed, edits)


if __name__ == '__main__':
    unittest.main()
