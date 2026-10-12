"""play_route save_fixture: a fixture saved from a run keeps its source fixture as "parent" and stays
marked edited when any ancestor was an edited save (Astra-2's Hunter's Dream position edit)."""
import contextlib
import hashlib
import importlib.util
import io
import json
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('play_route', ROOT / 'tools/macos/play_route.py')
play_route = importlib.util.module_from_spec(spec)
spec.loader.exec_module(play_route)


class SaveFixtureAncestry(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        base = Path(self.tmp.name)
        play_route.FIXTURES = base / 'saves'
        self.run_dir = base / 'run'
        (self.run_dir / 'user' / 'savedata' / 'x').mkdir(parents=True)
        (self.run_dir / 'user' / 'savedata' / 'x' / 'userdata0000').write_bytes(b'save')
        (self.run_dir / 'events.jsonl').write_text(json.dumps({'kind': 'capture', 'stage': 'dream-walk'}) + '\n')

    def tearDown(self):
        self.tmp.cleanup()

    def save(self, saves, name='out'):
        (self.run_dir / 'manifest.json').write_text(json.dumps(
            {'pid': None, 'output': '1080p', 'saves': saves,
             'artifacts': {'bb-probe': {'sha256': 'ab'}, 'note': 'not a hash'}}))
        with contextlib.redirect_stdout(io.StringIO()):
            play_route.save_fixture(self.run_dir, name)
        return json.loads((play_route.FIXTURES / name / 'fixture.json').read_text())

    def test_edited_ancestor_is_kept_and_flagged(self):
        # edited fixture -> a save from a run on it -> a save from a run on that one
        dream = {'name': 'hunters-dream-edited', 'edited': True, 'validation': 'pending',
                 'unverified': ['lamp unlocks'], 'edit_provenance': {'operation': 'position-only'}}
        walked = self.save({'fixture': 'hunters-dream-edited', 'from': dream}, 'dream-walked')
        self.assertEqual(walked['parent'], dream)
        self.assertIs(walked['edited'], True)
        self.assertEqual(walked['edited_origin'], 'hunters-dream-edited')
        self.assertEqual(walked['unverified'], ['lamp unlocks'])
        meta = self.save({'fixture': 'dream-walked', 'from': walked})
        self.assertEqual(meta['parent'], walked)
        self.assertEqual(meta['parent']['parent']['edit_provenance'], {'operation': 'position-only'})
        self.assertIs(meta['edited'], True)
        self.assertEqual(meta['edited_origin'], 'hunters-dream-edited')
        self.assertEqual(meta['unverified'], ['lamp unlocks'])
        self.assertEqual(meta['artifacts'], {'bb-probe': 'ab'})

    def test_played_ancestry_is_not_marked_edited(self):
        meta = self.save({'fixture': 'clinic-start', 'from': {'name': 'clinic-start', 'run': '/r'}})
        self.assertEqual(meta['parent'], {'name': 'clinic-start', 'run': '/r'})
        self.assertNotIn('edited', meta)

    def test_fresh_user_has_no_parent(self):
        meta = self.save('none (fresh user directory)')
        self.assertNotIn('parent', meta)
        self.assertNotIn('edited', meta)


class VerifyFixture(unittest.TestCase):
    """verify-fixture: narrow "verified" records about the fixture a run loaded; retiring an
    "unverified" entry keeps the former list in "unverified_history"."""
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        base = Path(self.tmp.name)
        play_route.FIXTURES = base / 'saves'
        self.dream = {'name': 'dream', 'edited': True, 'validation': 'pending',
                      'unverified': ['load/equip/attack/fire', 'save/reload', 'lamp unlocks']}
        (play_route.FIXTURES / 'dream' / 'savedata' / 'x').mkdir(parents=True)
        (play_route.FIXTURES / 'dream' / 'savedata' / 'x' / 'userdata0000').write_bytes(b'character')
        (play_route.FIXTURES / 'dream' / 'fixture.json').write_text(json.dumps(self.dream))
        self.run_dir = base / '20261012-000000-1080p-W'
        (self.run_dir / 'user' / 'savedata' / 'x').mkdir(parents=True)
        (self.run_dir / 'user' / 'savedata' / 'x' / 'userdata0000').write_bytes(b'played')
        (self.run_dir / 'events.jsonl').write_text('')
        (self.run_dir / 'manifest.json').write_text(json.dumps(
            {'pid': None, 'output': '1080p', 'saves': {'fixture': 'dream', 'from': self.dream},
             'artifacts': {'bb-probe': {'path': '/b/f87e4c0-PREVIEW-26/bb-probe', 'sha256': 'ab'},
                           'libbbgpu.dylib': {'path': '/b/f87e4c0-PREVIEW-26/gpu/libbbgpu.dylib', 'sha256': 'cd'}}}))

    def tearDown(self):
        self.tmp.cleanup()

    def meta(self, name='dream'):
        return json.loads((play_route.FIXTURES / name / 'fixture.json').read_text())

    def verify(self, *args):
        with contextlib.redirect_stdout(io.StringIO()):
            play_route.verify_fixture('dream', self.run_dir, *args)

    def test_record_names_the_loaded_fixture_and_build(self):
        self.verify(['pistol-fire'], 'bullets 20 -> 19', [], None)
        record, = self.meta()['verified']
        self.assertEqual(record['item'], 'pistol-fire')
        self.assertEqual(record['fixture'], 'dream')
        self.assertEqual(record['character_sha256'], hashlib.sha256(b'character').hexdigest())
        self.assertEqual(record['run'], '20261012-000000-1080p-W')
        self.assertEqual(record['build'], 'f87e4c0-PREVIEW-26')
        self.assertEqual(record['artifacts'], {'bb-probe': 'ab', 'libbbgpu.dylib': 'cd'})
        self.assertEqual(self.meta()['validation'], 'partial')
        self.assertEqual(self.meta()['unverified'], self.dream['unverified'])

    def test_retire_keeps_the_former_list(self):
        self.verify(['normal-save-quit', 'fresh-process-reload'], 'shots 18-04', ['save/reload'], 'both passed')
        meta = self.meta()
        self.assertEqual(meta['unverified'], ['load/equip/attack/fire', 'lamp unlocks'])
        self.assertEqual(meta['unverified_history'],
                         [{'items': self.dream['unverified'], 'reason': 'both passed', 'runs': ['20261012-000000-1080p-W']}])
        self.assertTrue(meta['edited'])

    def test_retire_rejects_unknown_entries_and_needs_a_reason(self):
        with self.assertRaises(SystemExit):
            self.verify(['load'], 'e', ['messenger gifts'], 'r')
        with self.assertRaises(SystemExit):
            self.verify(['load'], 'e', ['save/reload'], None)
        self.assertNotIn('verified', self.meta())

    def test_descendant_inherits_records_verbatim(self):
        self.verify(['pistol-fire'], 'bullets 20 -> 19', ['save/reload'], 'passed')
        dream = self.meta()
        (self.run_dir / 'manifest.json').write_text(json.dumps(
            {'pid': None, 'output': '1080p', 'saves': {'fixture': 'dream', 'from': dream},
             'artifacts': {'bb-probe': {'path': '/b/x/bb-probe', 'sha256': 'ef'}}}))
        with contextlib.redirect_stdout(io.StringIO()):
            play_route.save_fixture(self.run_dir, 'child')
        child = self.meta('child')
        for key in ('verified', 'unverified', 'unverified_history', 'validation'):
            self.assertEqual(child[key], dream[key])
        self.assertEqual(child['verified'][0]['fixture'], 'dream')


if __name__ == '__main__':
    unittest.main()
