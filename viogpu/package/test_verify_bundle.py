# SPDX-License-Identifier: MIT
"""Final archive identity tests; signature verification runs on real Windows."""
import json
import unittest

import flat_package as package
import test_flat_package as fixtures
import verify_bundle as bundle


class BundleTests(unittest.TestCase):
    def setUp(self):
        self.fixture = fixtures.ComposerTests()
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)
        self.output = self.fixture.root / "bundle"
        (self.output / "drivers").mkdir(parents=True)
        self.driver = self.output / "drivers/viogpu"
        self.fixture.driver.rename(self.driver)
        self.fixture.driver = self.driver
        self.fixture.inf = self.driver / "viogpuwddm.inf"
        self.parent = "c" * 40
        self.fixture.manifests["opengl"]["sources"].update(parent=self.parent, mesa_run=123)
        self.fixture.manifests["opencl"]["sources"].update(
            parent=self.parent, clvk_runtime_ci=456,
            compiler_original_sha256="79e236af8febd67fd02adfd93f81295c87e868e9fd861f71d03d1057e6be1f9d")
        self.fixture.save()
        self.fixture.assemble()
        package.finalize(self.driver)
        for name in bundle.DEBUG_FILES | {"viogpuwddm.cat"}:
            (self.driver / name).write_bytes(b"format fixture only")
        for name in bundle.INSTALLER_FILES:
            (self.output / name).write_bytes(b"format fixture only")

    def verify(self):
        return bundle.verify(self.output, self.parent, fixtures.MESA, 123,
                             fixtures.CLVK, 456, "100.6.101.58474")

    def change_manifest(self, update):
        path = self.driver / package.RECEIPT
        data = json.loads(path.read_text())
        update(data)
        path.write_text(json.dumps(data))

    def test_complete_bundle(self):
        receipt = self.verify()
        self.assertEqual(receipt["parent_commit"], self.parent)
        self.assertTrue(set(package.LOADER_PROBES.values()) <= receipt["gpu_files"].keys())
        self.assertIn("viogpu-flat-package.json", receipt["gpu_files"])
        self.assertIn("viogpu-install-native.cs", receipt["installer_files"])

    def test_reject_stale_producer(self):
        self.change_manifest(lambda m: m["sources"]["opencl"].update(parent="d" * 40))
        with self.assertRaisesRegex(ValueError, "Mixed producer"):
            self.verify()

    def test_reject_changed_signed_payload(self):
        (self.driver / "viogpucl_x64.dll").write_bytes(b"stale replacement")
        with self.assertRaisesRegex(ValueError, "Signed file changed"):
            self.verify()

    def test_reject_omitted_helper_inventory(self):
        self.change_manifest(lambda m: m["files"].pop(package.LOADER_PROBES["arm64"]))
        with self.assertRaisesRegex(ValueError, "INF copy inventory differ"):
            self.verify()

    def test_reject_missing_installer_helper(self):
        (self.output / "viogpu-install-certificates.psm1").unlink()
        with self.assertRaisesRegex(ValueError, "Missing installer root file"):
            self.verify()

    def test_reject_old_sidecar(self):
        (self.output / "opencl").mkdir()
        with self.assertRaisesRegex(ValueError, "Obsolete sidecar"):
            self.verify()

    def test_reject_uncopied_file(self):
        (self.driver / "extra.dll").write_bytes(b"extra")
        with self.assertRaisesRegex(ValueError, "Unexpected/missing flat"):
            self.verify()


if __name__ == "__main__":
    unittest.main()
