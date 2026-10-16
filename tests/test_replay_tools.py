import copy
import contextlib
import hashlib
import io
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest import mock
import subprocess
from types import SimpleNamespace

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

    def test_migration_rejects_direct_child_before_hashing_or_writing(self):
        source = self.root / 'protected'; source.mkdir()
        sentinel = source / 'unique-evidence'; sentinel.write_bytes(b'original')
        with mock.patch.object(identity.shutil, 'disk_usage', side_effect=AssertionError('guard must run first')), \
                self.assertRaisesRegex(ValueError, 'source|output'):
            identity.migrate(source, source / 'overlay', {})
        self.assertEqual(sentinel.read_bytes(), b'original')
        self.assertEqual(sorted(p.name for p in source.iterdir()), ['unique-evidence'])

    def test_migration_rejects_symlink_parent_into_protected_source(self):
        source = self.root / 'protected'; source.mkdir()
        sentinel = source / 'unique-evidence'; sentinel.write_bytes(b'original')
        alias = self.root / 'alias'; alias.symlink_to(source, target_is_directory=True)
        with mock.patch.object(identity.shutil, 'disk_usage', side_effect=AssertionError('guard must run first')), \
                self.assertRaisesRegex(ValueError, 'source|output'):
            identity.migrate(source, alias / 'overlay', {})
        self.assertEqual(sentinel.read_bytes(), b'original')
        self.assertEqual(sorted(p.name for p in source.iterdir()), ['unique-evidence'])
        self.assertTrue(alias.is_symlink())

    def test_migration_rejects_dangling_final_symlink(self):
        source = self.root / 'protected'; source.mkdir()
        target = self.root / 'other-project' / 'redirected'
        target.parent.mkdir()
        output = self.root / 'overlay'; output.symlink_to(target, target_is_directory=True)
        with mock.patch.object(identity.shutil, 'disk_usage', side_effect=AssertionError('guard must run first')), \
                self.assertRaisesRegex(ValueError, 'source|output'):
            identity.migrate(source, output, {})
        self.assertTrue(output.is_symlink())
        self.assertFalse(target.exists())

    def log(self, stop=False, duplicate=False, missing=False, busy=True, draws=(1487, 1487, 1487),
            done=True, name='run-1-A.log', collection=False):
        lines = ['Perf replay: 3 images restored; capture -> restore -> capture again is exact']
        for i in range(1, 4):
            if missing and i == 2: continue
            done_text = f', {50+i}.00 ms submit-to-GPU-done' if done else ''
            line = f'Perf replay iteration {i}: {40+i}.00 ms submit-to-idle{done_text}, GPU thread CPU 20.00 ms; {draws[i-1]} draws, 160 dispatches, 74 submissions; images registered 0, image uploads 57.8 MiB, buffer uploads 0.0 MiB'
            lines.append(line)
            if duplicate and i == 2: lines.append(line)
            if busy and i >= 2:
                lines.append(f'  GPU timestamp collection window at iteration {i} (74 submissions): 34.00 ms'
                             if collection else f'  GPU busy of iteration {i-1} (per-submission timestamps): 34.00 ms')
        lines.append('Perf replay steady state (iterations 2..3): done')
        if stop: lines.append('STOP: invalid last identity')
        path = self.root / name; path.write_text('\n'.join(lines)); return path

    def test_gpu_done_wall_is_distinct_from_collection_diagnostics(self):
        result = summary.summarize(self.log(), 3, True)
        self.assertEqual(result['samples'], 2)
        self.assertEqual(result['gpu_busy_samples'], 0)
        self.assertIsNone(result['gpu_busy_median_ms'])
        self.assertEqual(result['gpu_collection_window_median_ms'], 34)
        self.assertEqual(result['median_ms'], 52.5)
        self.assertEqual(result['wall_metric'], 'submit_to_gpu_done_wall_ms')
        self.assertFalse(result['timing_qualified'])

    def test_false_success_zero_or_incomplete_samples_reject(self):
        for kwargs in ({'stop': True}, {'duplicate': True}, {'missing': True}, {'busy': False}):
            with self.subTest(kwargs=kwargs), self.assertRaises(ValueError):
                summary.summarize(self.log(**kwargs), 3, True)
        path = self.root / 'empty'; path.write_text('')
        with self.assertRaises(ValueError): summary.summarize(path)
        result = summary.summarize(self.log(busy=False))
        self.assertIsNone(result['gpu_busy_median_ms'])

    def context(self, **changes):
        value = {'version': 1, 'source_gates': {'A': 'PASS', 'B': 'PASS'},
                 'qualification_only': False, 'diagnostic': False, 'diagnostic_reasons': [],
                 'comparison_status': 'MATCHED', 'reference_workload': [[1487, 160, 74]],
                 'expected_iterations': 3, 'destructive_probe': False}
        value.update(changes)
        return value

    def result(self, **log_args):
        return summary.summarize(self.log(**log_args), 3)

    def test_source_gate_status_and_explicit_qualification_only_survive_detachment(self):
        for gate in (None, 'NOT_RUN', 'SKIPPED', 'PASS'):
            for qualification_only in (False, True):
                context = self.context(source_gates={'A': 'PASS', 'B': gate},
                                       qualification_only=qualification_only)
                result = summary.qualify(self.result(), context)
                qualified = gate == 'PASS' and not qualification_only
                with self.subTest(gate=gate, qualification_only=qualification_only):
                    self.assertEqual(result['timing_qualified'], qualified)
                    if not qualified:
                        self.assertIn('SOURCE GATE UNQUALIFIED', summary.verdict_text(result))
                    self.assertEqual(result['qualification_context']['source_gates']['B'], gate)

    def test_trace_overrides_require_explicit_diagnostic_admission(self):
        self.assertEqual(ab.diagnostic_reasons({'BB_PERF_PHASES': '0'}), [])
        for key in ('BB_PERF_PHASES', 'BB_PASS_TRACE', 'BB_RING_TRACE', 'BB_REFRESH_TRACE'):
            self.assertEqual(ab.diagnostic_reasons({key: '1'}), [key])
        with mock.patch.object(sys, 'argv', ['runner', 'A', 'B', '--capture', '/unused',
                                            '--env', 'BB_PERF_PHASES=1']), \
                mock.patch.object(ab, 'build_manifest', return_value={'gate': 'PASS'}), \
                mock.patch.object(ab.shutil, 'disk_usage', return_value=SimpleNamespace(free=50 * 10**9)), \
                mock.patch.dict(ab.os.environ, {}, clear=True), self.assertRaisesRegex(ValueError, '--diagnostic'):
            ab.main()
        for context in (self.context(diagnostic=True), self.context(diagnostic_reasons=['BB_PASS_TRACE'])):
            result = summary.qualify(self.result(), context)
            self.assertIn('DIAGNOSTIC PERTURBED', summary.verdict_text(result))
            self.assertFalse(result['timing_qualified'])

    def test_enabled_replay_probe_requires_diagnostic_and_stays_incomparable(self):
        for value in ('', '0', 'off', 'OFF'):
            self.assertEqual(ab.diagnostic_reasons({'BB_REPLAY_GPU_PROBE': value}), [])
        self.assertEqual(ab.diagnostic_reasons({'BB_REPLAY_GPU_PROBE': 'scissor-1px'}), ['BB_REPLAY_GPU_PROBE'])
        with mock.patch.object(sys, 'argv', ['runner', 'A', 'B', '--capture', '/unused',
                                            '--env', 'BB_REPLAY_GPU_PROBE=scissor-1px']), \
                mock.patch.object(ab, 'build_manifest', return_value={'gate': 'PASS'}), \
                mock.patch.object(ab.shutil, 'disk_usage', return_value=SimpleNamespace(free=50 * 10**9)), \
                mock.patch.dict(ab.os.environ, {}, clear=True), self.assertRaisesRegex(ValueError, '--diagnostic'):
            ab.main()
        results = [self.result(name=f'run-{i}-{arm}.log') for i, arm in ((1, 'A'), (2, 'B'))]
        _, status, console = self.finish(results, self.context(diagnostic=True, destructive_probe=True))
        self.assertEqual(status, 2)
        self.assertIn('DIAGNOSTIC PERTURBED', console)
        self.assertIn('INCOMPARABLE', console)

    def test_raw_destructive_marker_overrides_clean_context(self):
        results = [self.result(name=f'run-{i}-{arm}.log') for i, arm in ((1, 'A'), (2, 'B'))]
        path = Path(results[0]['path'])
        path.write_text(path.read_text() + '\nprobe: INVALID_FOR_CORRECTNESS_QUALITY=1\n')
        results[0] = summary.summarize(path, 3)
        _, status, console = self.finish(results, self.context())
        self.assertEqual(status, 2)
        self.assertIn('INVALID FOR CORRECTNESS/QUALITY', console)
        self.assertTrue(all(not r['timing_qualified'] for r in results))
        detached = summary.summarize(path)
        self.assertTrue(detached['invalid_for_correctness_quality'])
        self.assertFalse(detached['timing_qualified'])

    def test_detached_expected_count_and_proofs_cannot_be_truncated(self):
        results = [self.result(name=f'run-{i}-{arm}.log') for i, arm in ((1, 'A'), (2, 'B'))]
        self.finish(results, self.context())
        path = Path(results[0]['path']); original = path.read_text()
        self.assertTrue(summary.summarize(path)['timing_qualified'])
        for removed in ('Perf replay iteration 3', 'capture -> restore -> capture again is exact',
                        'Perf replay steady state'):
            path.write_text('\n'.join(line for line in original.splitlines() if removed not in line))
            with self.subTest(removed=removed), self.assertRaises(ValueError):
                summary.summarize(path)
        path.write_text(original)
        with self.assertRaises(ValueError): summary.summarize(path, 4)
        context = self.context(); del context['expected_iterations']
        result = summary.qualify(self.result(name='unbounded.log'), context)
        self.assertFalse(result['timing_qualified'])
        self.assertIn('RUN EVIDENCE UNQUALIFIED', result['qualification'])

    def test_runner_refuses_preexisting_qualification_marker(self):
        results = [self.result(name=f'run-{i}-{arm}.log') for i, arm in ((1, 'A'), (2, 'B'))]
        path = Path(results[0]['path'])
        path.write_text(path.read_text() + '\n' + summary.VERDICT_PREFIX + json.dumps(self.context()) + '\n')
        with self.assertRaisesRegex(ValueError, 'preexisting qualification'):
            self.finish(results, self.context())
        self.assertFalse((self.root / 'results/results.json').exists())

    def finish(self, results, context):
        out = self.root / 'results'; out.mkdir()
        for number, result in enumerate(results, 1):
            result.update(run=number, arm='AB'[number - 1])
        console = io.StringIO()
        with contextlib.redirect_stdout(console):
            status = ab.finish_results(out, results, context)
        return out, status, console.getvalue()

    def test_matching_pass_arms_qualify_only_gpu_done_wall(self):
        results = [self.result(name=f'run-{i}-{arm}.log') for i, arm in ((1, 'A'), (2, 'B'))]
        out, status, console = self.finish(results, self.context())
        self.assertEqual(status, 0)
        self.assertIn('QUALIFIED RELATIVE ATTRIBUTION', console)
        for result in json.loads((out / 'results.json').read_text()):
            self.assertEqual(result['accepted_metrics'], ['submit_to_gpu_done_wall_ms'])
            detached = summary.summarize(Path(result['path']), 3)
            self.assertTrue(detached['timing_qualified'])
            self.assertIsNone(detached['gpu_busy_median_ms'])
        path = Path(results[0]['path'])
        path.write_text(path.read_text().replace('1487 draws', '1486 draws'))
        changed = summary.summarize(path, 3)
        self.assertIn('INCOMPARABLE', changed['qualification'])
        self.assertFalse(changed['timing_qualified'])

    def test_cross_arm_drift_retains_incomparable_results_and_nonzero(self):
        results = [self.result(name='run-1-A.log'),
                   self.result(name='run-2-B.log', draws=(1486, 1486, 1486))]
        out, status, console = self.finish(results, self.context(diagnostic=True))
        self.assertEqual(status, 2)
        self.assertIn('INCOMPARABLE', console)
        for result in json.loads((out / 'results.json').read_text()):
            self.assertFalse(result['timing_qualified'])
            self.assertEqual(result['comparison_status'], 'INCOMPARABLE')
            detached = summary.summarize(Path(result['path']), 3)
            self.assertIn('INCOMPARABLE', summary.verdict_text(detached))
        process = subprocess.run([sys.executable, '-B', str(Path(summary.__file__)), '--json',
                                  '--expected', '3', results[0]['path'], results[1]['path']],
                                 capture_output=True, text=True)
        self.assertEqual(process.returncode, 2)
        self.assertEqual(len(json.loads(process.stdout)), 2)

    def test_within_run_drift_is_retained_as_incomparable(self):
        result = self.result(draws=(1487, 1487, 1486))
        self.assertFalse(result['workload_consistent'])
        self.assertIn('INCOMPARABLE', result['qualification'])

    def test_qualification_only_status_in_console_results_and_detached_summary(self):
        results = [self.result(name=f'run-{i}-{arm}.log', busy=False) for i, arm in ((1, 'A'), (2, 'B'))]
        out, status, console = self.finish(results, self.context(qualification_only=True))
        self.assertEqual(status, 0)
        self.assertIn('SOURCE GATE UNQUALIFIED', console)
        self.assertIn('MISSING', console)
        for result in json.loads((out / 'results.json').read_text()):
            detached = summary.summarize(Path(result['path']), 3)
            self.assertIn('SOURCE GATE UNQUALIFIED', detached['qualification'])
            self.assertFalse(detached['timing_qualified'])

    def test_new_and_legacy_gpu_lines_never_qualify_per_iteration_busy(self):
        for collection in (False, True):
            result = self.result(collection=collection)
            self.assertEqual(result['gpu_collection_window_median_ms'], 34)
            self.assertIsNone(result['gpu_busy_median_ms'])
            row = result['gpu_collection_window_samples'][0]
            self.assertEqual(row['legacy_approximate_label'], not collection)
            self.assertEqual(row['submissions'], 74 if collection else None)

    def test_missing_or_zero_gpu_done_wall_cannot_qualify(self):
        with self.assertRaisesRegex(ValueError, 'missing submit-to-GPU-done'):
            self.result(done=False)
        path = self.log()
        path.write_text(path.read_text().replace('52.00 ms submit-to-GPU-done', '0.00 ms submit-to-GPU-done'))
        with self.assertRaisesRegex(ValueError, 'invalid wall time'):
            summary.summarize(path, 3)
        idle = summary.summarize(self.log(done=False))
        summary.qualify(idle, self.context())
        self.assertFalse(idle['timing_qualified'])
        self.assertEqual(idle['wall_metric'], 'submit_to_idle_wall_ms')

    def test_perf_schema_detects_unexpected_perturbation(self):
        path = self.root / 'perf.jsonl'
        for perturbed in (False, True):
            path.write_text(json.dumps({'type': 'schema', 'perturbed': perturbed}) + '\n')
            self.assertEqual(ab.perf_perturbed(path), perturbed)
        path.write_text('{}\n')
        with self.assertRaises(ValueError): ab.perf_perturbed(path)

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
        lib.write_bytes(b'library')
        for gate in (None, 'NOT_RUN', 'SKIPPED', 'FAILED'):
            value['qualification']['test_gate'] = gate
            manifest.write_text(json.dumps(value))
            with self.subTest(gate=gate):
                with self.assertRaises(ValueError): ab.build_manifest(manifest)
                if gate == 'FAILED':
                    with self.assertRaises(ValueError): ab.build_manifest(manifest, True)
                else:
                    self.assertNotEqual(ab.build_manifest(manifest, True)['gate'], 'PASS')


if __name__ == '__main__':
    unittest.main()
