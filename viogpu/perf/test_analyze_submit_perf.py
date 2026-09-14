# SPDX-License-Identifier: BSD-3-Clause
"""Synthetic accounting fixtures; none of these numbers is a hardware benchmark."""
import contextlib
import io
import json
from pathlib import Path
import re
import tempfile
import unittest

import analyze_submit_perf as perf


# Produce balanced drained snapshots, including a nonzero pre-capture baseline.
def snapshot(render=10, window=64, **overrides):
    record = dict(window=window, accepted=render + 3, dispatched=render + 2,
                  retired=render + 2, cancelled_queued=1, render=render,
                  bytes=render * 128, refs=render * 2, refs_max=2,
                  pending_peak=3, host_pending_peak=1, render_retired=render,
                  retire_100ns_total=render * 100, retire_100ns_max=100,
                  handoffs=render)
    record.update(overrides)
    return record


# Render the exact three-line diagnostic format with optional debugger prefixes.
def log_text(*records):
    result = []
    for record in records:
        for index, fields in enumerate(perf.FIELDS):
            prefix = '01:23:45 viogpu perf: ' if index == 0 else '01:23:45   '
            result.append(prefix + ' '.join(f'{name}={record[name]}' for name in fields))
    return '\n'.join(result) + '\n'


class SubmitPerfTests(unittest.TestCase):
    # Parse the same fixture format used by every accounting regression.
    def report(self, *records):
        return perf.summarize(perf.parse_records(log_text(*records).splitlines()))

    def test_interval_excludes_pre_capture_work(self):
        result = self.report(snapshot(100), snapshot(110, refs=230))
        self.assertEqual(result['interval']['counters']['render'], 10)
        self.assertEqual(result['interval']['refs_per_render'], 3)
        self.assertEqual(result['interval']['dispatch_to_terminal_us'], 10)

    def test_weighted_average_not_average_of_batch_averages(self):
        result = self.report(snapshot(10), snapshot(11, refs=30), snapshot(20, refs=48))
        self.assertAlmostEqual(result['interval']['refs_per_render'], 2.8)

    def test_uint64_precision_before_float_conversion(self):
        start = snapshot(1 << 54)
        end = snapshot((1 << 54) + 3)
        self.assertEqual(self.report(start, end)['interval']['counters']['render'], 3)

    def test_duplicate_snapshots_are_not_counted_twice(self):
        result = self.report(snapshot(), snapshot(), snapshot(20))
        self.assertEqual(result['duplicate_snapshots'], 1)
        self.assertEqual(result['interval']['counters']['render'], 10)

    def test_one_snapshot_has_no_interval(self):
        self.assertIsNone(self.report(snapshot())['interval'])

    def test_all_duplicate_snapshots_have_no_interval(self):
        self.assertIsNone(self.report(snapshot(), snapshot())['interval'])

    def test_no_render_interval_has_null_averages(self):
        first = snapshot()
        second = snapshot(accepted=14, cancelled_queued=2)
        result = self.report(first, second)
        self.assertIsNone(result['interval']['refs_per_render'])
        self.assertIsNone(result['interval']['dispatch_to_terminal_us'])
        self.assertNotIn('NaN', json.dumps(result, allow_nan=False))

    def test_peaks_are_preserved_as_lifetime_values(self):
        result = self.report(snapshot(refs_max=20), snapshot(20, refs_max=20))
        self.assertEqual(result['lifetime_peaks']['refs_max'], 20)
        self.assertNotIn('refs_max', result['interval']['counters'])

    def test_reset_or_wrap_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'decreased'):
            self.report(snapshot(20), snapshot(10))

    def test_decreasing_peak_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'decreased'):
            self.report(snapshot(pending_peak=4), snapshot(20, pending_peak=3))

    def test_mixed_windows_are_rejected(self):
        with self.assertRaisesRegex(ValueError, 'mixed windows'):
            self.report(snapshot(window=1), snapshot(20, window=64))

    def test_reordered_snapshots_are_rejected(self):
        with self.assertRaises(ValueError):
            self.report(snapshot(), snapshot(30), snapshot(20))

    def test_truncated_record_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'truncated'):
            perf.parse_records(log_text(snapshot()).splitlines()[:2])

    def test_interleaved_headers_are_rejected(self):
        lines = log_text(snapshot()).splitlines()
        with self.assertRaisesRegex(ValueError, 'interrupts'):
            perf.parse_records([lines[0], lines[0], lines[1], lines[2]])

    def test_out_of_order_rows_are_rejected(self):
        lines = log_text(snapshot()).splitlines()
        with self.assertRaisesRegex(ValueError, 'out-of-order'):
            perf.parse_records([lines[0], lines[2], lines[1]])

    def test_unrelated_log_lines_can_be_interspersed(self):
        lines = log_text(snapshot()).splitlines()
        self.assertEqual(len(perf.parse_records([lines[0], 'ordinary debug text', *lines[1:]])), 1)

    def test_unknown_duplicate_negative_and_overflow_fields(self):
        text = log_text(snapshot())
        for replacement in ('refs=20 refs=20', 'refs=-1', 'refs=' + str(1 << 64),
                            'refs=twenty', 'unknown=20'):
            with self.subTest(replacement=replacement), self.assertRaises(ValueError):
                perf.parse_records(text.replace('refs=20', replacement).splitlines())

    def test_missing_field_is_rejected(self):
        with self.assertRaises(ValueError):
            perf.parse_records(log_text(snapshot()).replace('refs=20 ', '').splitlines())

    def test_empty_and_unrelated_input_are_rejected(self):
        for lines in ([], ['ordinary logs only']):
            with self.assertRaises(ValueError):
                perf.parse_records(lines)

    def test_accounting_violations_are_rejected(self):
        for override in ({'accepted': 99}, {'dispatched': 99}, {'render_retired': 9},
                         {'handoffs': 11}, {'window': 0}, {'window': 129},
                         {'host_pending_peak': 65}, {'refs_max': 21},
                         {'retire_100ns_max': 1001}):
            with self.subTest(override=override), self.assertRaises(ValueError):
                self.report(snapshot(**override))

    def test_comparison_reports_observations_without_fps_verdict(self):
        baseline = self.report(snapshot(window=1), snapshot(20, window=1))
        candidate = self.report(snapshot(), snapshot(20, retire_100ns_total=1500))
        result = perf.compare(baseline, candidate)
        self.assertEqual(result['observations']['dispatch_to_terminal_us']['relative_change_percent'], -50)
        self.assertIsNone(result['performance_verdict'])

    def test_comparison_rejects_missing_interval(self):
        with self.assertRaises(ValueError):
            perf.compare(self.report(snapshot()), self.report(snapshot(), snapshot(20)))

    def test_comparison_rejects_idle_interval(self):
        idle = self.report(snapshot(), snapshot(accepted=14, cancelled_queued=2))
        with self.assertRaises(ValueError):
            perf.compare(idle, idle)

    def test_zero_baseline_metric_has_no_relative_percentage(self):
        baseline = self.report(snapshot(handoffs=0), snapshot(20, handoffs=0))
        candidate = self.report(snapshot(), snapshot(20))
        result = perf.compare(baseline, candidate)
        self.assertIsNone(result['observations']['pipeline_handoff_fraction']['relative_change_percent'])

    def test_cli_json_bom_and_input_overwrite_guard(self):
        with tempfile.TemporaryDirectory() as directory:
            capture = Path(directory) / 'capture.log'
            output = Path(directory) / 'result.json'
            capture.write_text(log_text(snapshot(), snapshot(20)), encoding='utf-8-sig')
            self.assertEqual(perf.main([str(capture), '--output', str(output)]), 0)
            self.assertEqual(json.loads(output.read_text())['schema_version'], 1)
            original = capture.read_bytes()
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(perf.main([str(capture), '--output', str(capture)]), 2)
            self.assertEqual(capture.read_bytes(), original)

    def test_cli_error_does_not_write_partial_output(self):
        with tempfile.TemporaryDirectory() as directory:
            capture = Path(directory) / 'bad.log'
            output = Path(directory) / 'output.json'
            capture.write_text('viogpu perf: window=64\n')
            output.write_text('retained')
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(perf.main([str(capture), '--output', str(output)]), 2)
            self.assertEqual(output.read_text(), 'retained')

    def test_inconsistent_interval_handoffs_are_rejected(self):
        with self.assertRaisesRegex(ValueError, 'interval handoffs'):
            self.report(snapshot(handoffs=0), snapshot(11, handoffs=11))

    def test_inconsistent_idle_interval_measurements_are_rejected(self):
        with self.assertRaisesRegex(ValueError, 'without a Render'):
            self.report(snapshot(), snapshot(accepted=14, cancelled_queued=2, refs=21))

    def test_schema_matches_production_reporter(self):
        root = Path(__file__).resolve().parents[2]
        source = (root / 'viogpu/viogpudo/viogpudo.cpp').read_text()
        function = source.split('VOID VioGpuDod::ReportNativeSubmitPerf(void)', 1)[1].split(
            'BOOLEAN VioGpuDod::OpenNativePassiveQueue(void)', 1)[0]
        names = re.findall(r'([a-z0-9_]+)=%(?:llu|u)', function)
        self.assertEqual(names, [name for row in perf.FIELDS for name in row])


if __name__ == '__main__':
    unittest.main()
