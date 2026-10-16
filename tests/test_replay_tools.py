import copy
import hashlib
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools/macos'))
import replay_identity as identity
import replay_summary as summary
import replay_ab as ab


class ReplayTools(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.plan = {'objects': [{'old': {'start': 0x1000, 'size': 0x100, 'ordinal': 1,
                                      'module': 'libbbgpu.dylib', 'offset': 123},
                                 'new': {'name': 'labels', 'start': 0x1020, 'size': 0x80},
                                 'evidence': 'matching binary disassembly proves this subrange'}]}
        self.raw = (identity.HEADER.pack(b'BBRPRSET', 2, 0, 2) +
                    identity.RESET.pack(0x100, 4, 1, 0x37, 0) + b'abcd' +
                    identity.RESET.pack(0x1028, 4, 1, 0x37, 1) + b'efgh' +
                    identity.OLD.pack(0x1000, 0x100, 1, 123, b'libbbgpu.dylib'))

    def test_transform_preserves_payloads_and_explicit_offset(self):
        result = identity.transform(self.raw, self.plan)
        self.assertEqual(identity.HEADER.unpack_from(result)[1], 3)
        self.assertIn(b'abcd', result); self.assertIn(b'efgh', result)
        obj = identity.NAMED.unpack_from(result, len(result) - identity.NAMED.size)
        self.assertEqual(obj[:3], (0x1020, 0x80, 8))
        self.assertEqual(obj[3].split(b'\0')[0], b'labels')
        self.assertEqual(len(self.raw) - len(result), 8)

    def test_bad_legacy_or_semantic_mapping_rejects(self):
        for field, value in [('ordinal', 2), ('offset', 124), ('size', 0x101), ('module', 'other')]:
            plan = copy.deepcopy(self.plan); plan['objects'][0]['old'][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                identity.transform(self.raw, plan)
        for field, value in [('name', ''), ('name', 'x' * 64), ('start', 0x1030), ('size', 0x200)]:
            plan = copy.deepcopy(self.plan); plan['objects'][0]['new'][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                identity.transform(self.raw, plan)

    def test_truncation_trailing_old_version_duplicate_and_evidence(self):
        bad_version = bytearray(self.raw); struct.pack_into('<I', bad_version, 8, 3)
        for raw in (self.raw[:-1], self.raw + b'x', self.raw[:20], bad_version):
            with self.assertRaises(ValueError): identity.transform(raw, self.plan)
        for missing in ('evidence',):
            plan = copy.deepcopy(self.plan); del plan['objects'][0][missing]
            with self.assertRaises(ValueError): identity.transform(self.raw, plan)
        plan = copy.deepcopy(self.plan); plan['objects'].append(copy.deepcopy(plan['objects'][0]))
        with self.assertRaises(ValueError): identity.transform(self.raw, plan)

    def log(self, stop=False, duplicate=False, missing=False, busy=True):
        lines = ['Perf replay: 3 images restored; capture -> restore -> capture again is exact']
        for i in range(1, 4):
            if missing and i == 2: continue
            line = f'Perf replay iteration {i}: {40+i}.00 ms submit-to-idle, GPU thread CPU 20.00 ms; 1487 draws, 160 dispatches, 74 submissions; images registered 0, image uploads 57.8 MiB, buffer uploads 0.0 MiB'
            lines.append(line)
            if duplicate and i == 2: lines.append(line)
            if busy and i >= 2: lines.append(f'  GPU busy of iteration {i-1} (per-submission timestamps): 34.00 ms')
        lines.append('Perf replay steady state (iterations 2..3): done')
        if stop: lines.append('STOP: invalid last identity')
        path = self.root / 'run-1-A.log'; path.write_text('\n'.join(lines)); return path

    def test_complete_summary_requires_delayed_gpu_samples(self):
        result = summary.summarize(self.log(), 3, True)
        self.assertEqual(result['samples'], 2)
        self.assertEqual(result['gpu_busy_samples'], 1)
        self.assertEqual(result['gpu_busy_median_ms'], 34)
        self.assertEqual(result['median_ms'], 42.5)

    def test_false_success_zero_or_incomplete_samples_reject(self):
        for kwargs in ({'stop': True}, {'duplicate': True}, {'missing': True}, {'busy': False}):
            with self.subTest(kwargs=kwargs), self.assertRaises(ValueError):
                summary.summarize(self.log(**kwargs), 3, True)
        path = self.root / 'empty'; path.write_text('')
        with self.assertRaises(ValueError): summary.summarize(path)
        result = summary.summarize(self.log(busy=False))
        self.assertIsNone(result['gpu_busy_median_ms'])

    def test_env_is_parsed_without_shell_and_protected_values_reject(self):
        self.assertEqual(ab.env_items(['BB_FOO="a b"']), {'BB_FOO': 'a b'})
        for value in ('PATH=x', 'BB_PERF=0', 'BB_REPLAY_RUN=x', 'BB_HEAVY_DISK_BUDGET_GB=1'):
            with self.subTest(value=value), self.assertRaises(ValueError): ab.env_items([value])

    def test_build_manifest_hashes_exact_companion_and_gate(self):
        probe = self.root / 'bb-probe'; probe.write_bytes(b'test executable'); probe.chmod(0o700)
        lib = self.root / 'gpu/libbbgpu.dylib'; lib.parent.mkdir(); lib.write_bytes(b'library')
        manifest = self.root / 'manifest.json'
        value = {'source_tree': {'head': 'a' * 40}, 'qualification': {'test_gate': 'PASS'},
                 'artifacts': {name: {'path': str(path), 'sha256': identity.digest(path)}
                               for name, path in [('bb-probe', probe), ('libbbgpu.dylib', lib)]}}
        manifest.write_text(json.dumps(value)); self.assertEqual(ab.build_manifest(manifest)['gate'], 'PASS')
        lib.write_bytes(b'changed')
        with self.assertRaises(ValueError): ab.build_manifest(manifest)
        value['qualification']['test_gate'] = 'NOT_RUN'; manifest.write_text(json.dumps(value))
        with self.assertRaises(ValueError): ab.build_manifest(manifest, True)


if __name__ == '__main__':
    unittest.main()
