#!/usr/bin/env python3
import importlib.util
import json
import subprocess
import sys
import unittest
from pathlib import Path


SCRIPT = Path(__file__).with_name("classify-crosvm-context-diagnostics.py")
SPEC = importlib.util.spec_from_file_location("context_classifier", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


def complete_trace(ctx_id, slice_idx):
    bit = 1 << slice_idx
    return [
        f"VA slice {slice_idx}: [0x1000, +8GB)",
        f"virgl context table add: ctx_id={ctx_id} result=ok",
        f"rutabaga context destroy begin: ctx_id={ctx_id}",
        f"virglrenderer context drop begin: ctx_id={ctx_id} capset_id=6",
        f"virgl context table remove begin: ctx_id={ctx_id} found=1",
        f"virgl context backend destroy begin: ctx_id={ctx_id}",
        f"ctx_id={ctx_id} backend destroy begin VA slice={slice_idx}",
        f"ctx_id={ctx_id} released VA slice {slice_idx} used=0x{bit:016x}->0x0000000000000000",
        f"ctx_id={ctx_id} backend destroy complete",
        f"virgl context backend destroy complete: ctx_id={ctx_id}",
        f"virgl context table remove complete: ctx_id={ctx_id} found=1",
        f"virglrenderer context drop complete: ctx_id={ctx_id} capset_id=6",
        f"rutabaga context destroy complete: ctx_id={ctx_id}",
    ]


class ClassifierTests(unittest.TestCase):
    def classify(self, lines, expected=2):
        return MODULE.classify_text("\n".join(lines), expected_created=expected)

    def test_two_complete_contexts_pass(self):
        result = self.classify(complete_trace(10, 0) + complete_trace(11, 1))
        self.assertTrue(result["passed"])
        self.assertEqual(
            result["classification"],
            "all_created_contexts_destroyed_and_released",
        )

    def test_missing_rutabaga_destroy_is_first_boundary(self):
        lines = complete_trace(10, 0)
        lines += ["VA slice 1: [0x2000, +8GB)", "virgl context table add: ctx_id=11 result=ok"]
        result = self.classify(lines)
        self.assertFalse(result["passed"])
        self.assertEqual(result["contexts"][1]["first_missing"], "rutabaga_destroy_begin")

    def test_missing_rust_drop_is_first_boundary(self):
        lines = complete_trace(10, 0)
        lines += [
            "VA slice 1: [0x2000, +8GB)",
            "virgl context table add: ctx_id=11 result=ok",
            "rutabaga context destroy begin: ctx_id=11",
        ]
        result = self.classify(lines)
        self.assertEqual(result["contexts"][1]["first_missing"], "virglrenderer_drop_begin")

    def test_table_lookup_miss_is_distinguished(self):
        lines = complete_trace(10, 0)
        lines += [
            "VA slice 1: [0x2000, +8GB)",
            "virgl context table add: ctx_id=11 result=ok",
            "rutabaga context destroy begin: ctx_id=11",
            "virglrenderer context drop begin: ctx_id=11 capset_id=6",
            "virgl context table remove begin: ctx_id=11 found=0",
            "virgl context table remove complete: ctx_id=11 found=0",
            "virglrenderer context drop complete: ctx_id=11 capset_id=6",
            "rutabaga context destroy complete: ctx_id=11",
        ]
        result = self.classify(lines)
        self.assertEqual(result["contexts"][1]["first_missing"], "virgl_context_table_lookup")

    def test_missing_release_is_distinguished(self):
        lines = complete_trace(10, 0)
        lines += [
            "VA slice 1: [0x2000, +8GB)",
            "virgl context table add: ctx_id=11 result=ok",
            "rutabaga context destroy begin: ctx_id=11",
            "virglrenderer context drop begin: ctx_id=11 capset_id=6",
            "virgl context table remove begin: ctx_id=11 found=1",
            "virgl context backend destroy begin: ctx_id=11",
            "ctx_id=11 backend destroy begin VA slice=1",
        ]
        result = self.classify(lines)
        self.assertEqual(result["contexts"][1]["first_missing"], "kgsl_va_slice_release")

    def test_release_bit_must_be_cleared(self):
        lines = complete_trace(10, 0) + complete_trace(11, 1)
        release_index = lines.index(
            "ctx_id=11 released VA slice 1 used=0x0000000000000002->0x0000000000000000"
        )
        lines[release_index] = (
            "ctx_id=11 released VA slice 1 "
            "used=0x0000000000000002->0x0000000000000002"
        )
        result = self.classify(lines)
        self.assertEqual(result["contexts"][1]["first_missing"], "va_slice_bit_clear")

    def test_backend_destroy_slice_must_match_claim(self):
        lines = complete_trace(10, 0) + complete_trace(11, 1)
        begin_index = lines.index("ctx_id=11 backend destroy begin VA slice=1")
        lines[begin_index] = "ctx_id=11 backend destroy begin VA slice=2"
        result = self.classify(lines)
        self.assertEqual(
            result["contexts"][1]["first_missing"],
            "drm2kgsl_va_slice_identity",
        )

    def test_table_remove_complete_must_still_find_context(self):
        lines = complete_trace(10, 0) + complete_trace(11, 1)
        complete_index = lines.index(
            "virgl context table remove complete: ctx_id=11 found=1"
        )
        lines[complete_index] = (
            "virgl context table remove complete: ctx_id=11 found=0"
        )
        result = self.classify(lines)
        self.assertEqual(
            result["contexts"][1]["first_missing"],
            "virgl_context_table_remove_complete_lookup",
        )

    def test_claims_pair_with_successful_table_adds(self):
        result = self.classify(complete_trace(41, 3) + complete_trace(42, 5))
        self.assertEqual([item["slice"] for item in result["contexts"]], [3, 5])

    def test_preexisting_context_is_not_a_candidate(self):
        lines = [
            "rutabaga context destroy begin: ctx_id=7",
            "virglrenderer context drop begin: ctx_id=7 capset_id=6",
        ]
        lines += complete_trace(10, 0) + complete_trace(11, 1)
        result = self.classify(lines)
        self.assertTrue(result["passed"])
        self.assertEqual(result["external_context_ids"], [7])

    def test_created_count_mismatch_is_structural(self):
        result = self.classify(complete_trace(10, 0))
        self.assertFalse(result["passed"])
        self.assertEqual(result["classification"], "structural_capture_error")
        self.assertIn("created context count is 1, expected 2", result["anomalies"])

    def test_capture_helper_headers_and_line_prefixes_are_accepted(self):
        trace = complete_trace(10, 0) + complete_trace(11, 1)
        lines = [
            "PID=22756",
            "CONTEXT_TABLE_ADD=2",
            "===== FILTERED_CONTEXT_TEARDOWN =====",
        ]
        lines.extend(f"{index}:{line}" for index, line in enumerate(trace, 1))
        lines.append("CAPTURE_COMPLETE=1")
        result = self.classify(lines)
        self.assertTrue(result["passed"])

    def run_cli(self, lines, expected=1):
        return subprocess.run(
            [
                sys.executable,
                str(SCRIPT),
                "-",
                "--expected-created",
                str(expected),
            ],
            input="\n".join(lines),
            text=True,
            capture_output=True,
            check=False,
        )

    def test_cli_returns_zero_for_complete_trace(self):
        completed = self.run_cli(complete_trace(10, 0))
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertTrue(json.loads(completed.stdout)["passed"])

    def test_cli_returns_one_for_missing_boundary(self):
        incomplete = self.run_cli(
            ["VA slice 0: [0x1000, +8GB)", "virgl context table add: ctx_id=10 result=ok"]
        )
        self.assertEqual(incomplete.returncode, 1, incomplete.stderr)
        self.assertEqual(
            json.loads(incomplete.stdout)["contexts"][0]["first_missing"],
            "rutabaga_destroy_begin",
        )

    def test_cli_returns_two_for_structural_capture_error(self):
        structural = self.run_cli([], expected=1)
        self.assertEqual(structural.returncode, 2, structural.stderr)
        self.assertEqual(
            json.loads(structural.stdout)["classification"],
            "structural_capture_error",
        )


if __name__ == "__main__":
    unittest.main()
