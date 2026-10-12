"""play_route save_fixture: a fixture saved from a run keeps its source fixture as "parent" and stays
marked edited when any ancestor was an edited save (Astra-2's Hunter's Dream position edit)."""
import contextlib
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


if __name__ == '__main__':
    unittest.main()
