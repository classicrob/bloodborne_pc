import copy
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('frame_summary', Path(__file__).resolve().parents[1] /
                                            'tools/macos/frame_summary.py')
summary = importlib.util.module_from_spec(spec)
spec.loader.exec_module(summary)


class RawFlipSummary(unittest.TestCase):
    def records(self):
        return [
            {'type': 'schema', 'version': 3, 'frame_samples': True, 'perturbed': False},
            {'type': 'queue'},
            {'type': 'window', 'perturbed': False, 'flips': 3, 'flip_sequence_end': 3,
             'flip_samples': [[20_000_000, 10_000_000], [40_000_000, 20_000_000]]},
            {'type': 'window', 'perturbed': False, 'flips': 2, 'flip_sequence_end': 5, 'final': True,
             'flip_samples': [[90_000_000, 50_000_000], [200_000_000, 110_000_000]]},
        ]

    def test_exact_pooled_distribution_and_thresholds(self):
        result = summary.summarize(self.records())
        self.assertEqual((result['median_ms'], result['p95_ms'], result['p99_ms']), (20, 110, 110))
        self.assertEqual((result['over_60hz_budget'], result['over_50ms'], result['over_100ms']), (3, 1, 1))
        self.assertAlmostEqual(result['one_percent_low_fps'], 1000 / 110)
        self.assertAlmostEqual(result['fps'], 4 / .19)
        self.assertEqual(result['first_flip_without_interval'], 1)

    def test_bounds_exclude_crossing_intervals(self):
        result = summary.summarize(self.records(), .03, .2)
        self.assertEqual(result['intervals_selected'], 2)
        self.assertEqual(result['intervals_outside_bounds'], 2)
        self.assertEqual(result['median_ms'], 50)

    def test_rejects_loss_bad_clock_and_perturbed_data(self):
        for mutation in ('sequence', 'count', 'clock', 'perturbed', 'raw_off'):
            records = copy.deepcopy(self.records())
            if mutation == 'sequence': records[-1]['flip_sequence_end'] += 1
            if mutation == 'count': records[-1]['flip_samples'].pop()
            if mutation == 'clock': records[-1]['flip_samples'][0][0] += 1
            if mutation == 'perturbed': records[0]['perturbed'] = True
            if mutation == 'raw_off': records[0]['frame_samples'] = False
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                summary.summarize(records)

    def test_missing_final_is_explicit(self):
        records = self.records()
        records[-1]['final'] = False
        self.assertFalse(summary.summarize(records)['final_window_present'])


if __name__ == '__main__':
    unittest.main()
