# SPDX-License-Identifier: MIT
"""Keep KMD hot-path source transforms fail-closed as the driver evolves."""
from pathlib import Path
import subprocess
import sys
import unittest


class PerfTransformTests(unittest.TestCase):
    def test_reviewed_source_shape_is_unique(self):
        root = Path(__file__).resolve().parents[2]
        script = root / "viogpu/perf/apply_perf_cpp.py"
        result = subprocess.run(
            [sys.executable, str(script), "--check", "--root", str(root)],
            check=False,
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PASS KMD hot-path transform shape", result.stdout)

    def test_wddm_project_applies_transform_before_compile(self):
        root = Path(__file__).resolve().parents[2]
        project = (root / "viogpu/viogpuwddm/viogpuwddm.vcxproj").read_text(encoding="utf-8")
        self.assertIn('Name="ApplyDroidVmPerfCpp" BeforeTargets="ClCompile"', project)
        self.assertIn('..\\perf\\apply_perf_cpp.py', project)


if __name__ == "__main__":
    unittest.main()
