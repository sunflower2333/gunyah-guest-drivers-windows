# SPDX-License-Identifier: MIT
"""Keep legacy perf migrations fail-closed; compile only checked-in C++ source."""
from contextlib import redirect_stdout
import importlib.util
import io
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('perf_migration', ROOT / 'viogpu/perf/apply_perf_cpp.py')
MIGRATION = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MIGRATION)


class PerfTransformTests(unittest.TestCase):
    def test_reviewed_source_shape_is_unique(self):
        result = subprocess.run(
            [sys.executable, str(ROOT / 'viogpu/perf/apply_perf_cpp.py'), '--check', '--root', str(ROOT)],
            check=False, capture_output=True, text=True,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('PASS KMD hot-path transform shape', result.stdout)

    def test_compilation_does_not_mutate_source(self):
        project = (ROOT / 'viogpu/viogpuwddm/viogpuwddm.vcxproj').read_text(encoding='utf-8')
        self.assertNotIn('ApplyDroidVmPerfCpp', project)
        self.assertNotIn('apply_perf_cpp.py', project)
        self.assertIn('VIOGPU_NATIVE_PIPELINE_WINDOW=$(VioGpuNativePipelineWindow)', project)
        MIGRATION.validate_applied()

    def test_insert_delete_and_replace_are_idempotent(self):
        for old, new in [('anchor', 'prefix anchor'), ('prefix anchor', 'anchor'), ('old block', 'new block')]:
            with self.subTest(old=old, new=new), tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / 'test.cpp'
                path.write_text(old, encoding='utf-8')
                with redirect_stdout(io.StringIO()):
                    MIGRATION.replace_once(path, old, new, 'fixture')
                    self.assertEqual(path.read_text(), new)
                    MIGRATION.replace_once(path, old, new, 'fixture')
                    self.assertEqual(path.read_text(), new)

    def test_duplicate_mixed_and_missing_anchors_fail_closed(self):
        for old, new in [('anchor', 'prefix anchor'), ('prefix anchor', 'anchor'), ('old block', 'new block')]:
            for text in [old + old, new + new, old + new, 'unrelated source']:
                with self.subTest(text=text), tempfile.TemporaryDirectory() as directory:
                    path = Path(directory) / 'test.cpp'
                    path.write_text(text, encoding='utf-8')
                    with self.assertRaises(RuntimeError):
                        MIGRATION.replace_once(path, old, new, 'fixture')
                    self.assertEqual(path.read_text(), text)

    def test_reviewed_applied_variants_are_preserved(self):
        for text in ['new block', 'new\nblock']:
            for check_only in [False, True]:
                with self.subTest(text=text, check=check_only), tempfile.TemporaryDirectory() as directory:
                    path = Path(directory) / 'test.cpp'
                    path.write_text(text, encoding='utf-8')
                    try:
                        MIGRATION.CHECK_ONLY = check_only
                        with redirect_stdout(io.StringIO()):
                            MIGRATION.replace_once(path, 'old block', 'new block', 'fixture',
                                                   applied_variants=('new\nblock',))
                        self.assertEqual(path.read_text(), text)
                    finally:
                        MIGRATION.CHECK_ONLY = False

    def test_variant_duplicates_mixed_and_semantic_changes_fail_closed(self):
        for text in ['new\nblock' * 2, 'new block new\nblock',
                     'old block new\nblock', 'new changed block', 'missing block']:
            with self.subTest(text=text), tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / 'test.cpp'
                path.write_text(text, encoding='utf-8')
                with self.assertRaises(RuntimeError):
                    MIGRATION.replace_once(path, 'old block', 'new block', 'fixture',
                                           applied_variants=('new\nblock',))
                self.assertEqual(path.read_text(), text)

    def test_check_only_preserves_unpatched_bytes(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'test.cpp'
            path.write_text('old block', encoding='utf-8')
            try:
                MIGRATION.CHECK_ONLY = True
                with redirect_stdout(io.StringIO()):
                    MIGRATION.replace_once(path, 'old block', 'new block', 'fixture')
                self.assertEqual(path.read_text(), 'old block')
            finally:
                MIGRATION.CHECK_ONLY = False


if __name__ == '__main__':
    unittest.main()
