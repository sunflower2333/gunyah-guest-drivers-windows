#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Exercise the real package composer with format fixtures, not GPU/ABI claims."""
import hashlib
import json
from pathlib import Path
import struct
import tempfile
import unittest

import flat_package as package

MESA = "a" * 40
CLVK = "b" * 40


def pe(machine):
    # Deliberately not executable. Actual linker, dual-view and dependency
    # validation belong to Windows CI; these tests exercise packaging rules.
    data = bytearray(256)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, 64)
    data[64:68] = b"PE\0\0"
    struct.pack_into("<H", data, 68, package.MACHINES[machine])
    return bytes(data)


class ComposerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.driver, self.gl, self.cl = (self.root / name for name in ("driver", "gl", "cl"))
        for directory in (self.driver, self.gl, self.cl):
            directory.mkdir()
        source = Path(__file__).parents[1] / "viogpuwddm/viogpuwddm.inx"
        self.inf = self.driver / "viogpuwddm.inf"
        self.inf.write_text(source.read_text().replace("INX_PLATFORM_DRIVERS_DIR", "13")
                            .replace("$ARCH$", "ARM64").replace("0.1.0.0", "100.6.101.58474"))
        (self.driver / "viogpuwddm.sys").write_bytes(pe("arm64"))
        (self.driver / "viogpud3d.dll").write_bytes(pe("arm64"))
        self.manifests = {
            "opengl": {"schema": 1, "family": "opengl", "sources": {"mesa": MESA}, "files": {}},
            "opencl": {"schema": 1, "family": "opencl", "sources": {"clvk": CLVK}, "files": {}},
        }
        for arch in package.ARCHES:
            for stem in ("gl", "egl", "gles1", "gles2", "gl_vk", "gl_loader"):
                self.add("opengl", f"viogpu_{stem}_{arch}.dll", arch, "runtime")
            for name in (f"viogpucl_{arch}.dll", f"viogpucl_vk_{arch}.dll"):
                self.add("opencl", name, arch, "runtime")
        for _, (family, name, machine, _) in package.REGISTRATION.items():
            if name.endswith(".json"):
                proxy = "viogpuopengl_x86.dll" if "wow" in name else "viogpuopengl.dll"
                self.add(family, name, "data", "data", json.dumps({
                    "file_format_version": "1.0.0", "ICD": {"library_path": ".\\" + proxy}
                }).encode())
            else:
                self.add(family, name, machine, "icd")
        self.add("opencl", "viogpu_clspv_x64.exe", "x64", "compiler")
        self.add("opencl", "OpenCL.dll", "arm64x", "system-loader")
        self.add("opencl", "OpenCL32.dll", "x86", "system-loader")
        self.add("opencl", "control_x86.exe", "x86", "probe")
        self.manifests["opencl"]["loader_probes"] = dict(package.LOADER_PROBES)
        for arch, name in package.LOADER_PROBES.items():
            self.add("opencl", name, arch, "installer-helper")
        self.save()

    def add(self, family, name, machine, role, content=None):
        directory = self.gl if family == "opengl" else self.cl
        data = content if content is not None else pe(machine)
        (directory / name).write_bytes(data)
        self.manifests[family]["files"][name] = {
            "sha256": hashlib.sha256(data).hexdigest(), "machine": machine, "role": role}

    def save(self):
        for family, path in (("opengl", self.gl), ("opencl", self.cl)):
            (path / package.MANIFEST).write_text(json.dumps(self.manifests[family]))

    def assemble(self):
        return package.assemble(self.driver, self.gl, self.cl, MESA, CLVK)

    def snapshot(self):
        return {p.name: p.read_bytes() for p in self.driver.iterdir()}

    def reject_unchanged(self, message):
        before = self.snapshot()
        with self.assertRaisesRegex(ValueError, message):
            self.assemble()
        self.assertEqual(before, self.snapshot())

    def test_full_inventory_same_inf_same_directory(self):
        receipt = self.assemble()
        text = self.inf.read_text()
        copied = package.source_files(text)
        self.assertEqual(set(copied), set(receipt["api_files_before_signing"]) |
                         {"viogpuwddm.sys", "viogpud3d.dll", package.RECEIPT})
        self.assertNotIn("control_x86.exe", copied)
        self.assertIn("OpenCL.dll", copied)
        self.assertIn("OpenCL32.dll", copied)
        self.assertTrue(set(package.LOADER_PROBES.values()) <= set(copied))
        self.assertTrue(all(p.is_file() for p in self.driver.iterdir()))
        for key, (_, name, _, flags) in package.REGISTRATION.items():
            self.assertIn(f'HKR,,{key},{flags},"%13%\\{name}"', text)
        self.assertNotIn("Program Files", text)
        self.assertNotIn("SOFTWARE\\Khronos", text)

    def test_catalog_manifest_inventory_uses_post_signing_hashes(self):
        self.assemble()
        image = self.driver / "viogpucl_arm64.dll"
        old_hash = package.sha(image)
        image.write_bytes(image.read_bytes() + b"simulated signing changes")
        receipt = package.finalize(self.driver)
        self.assertEqual(receipt["phase"], "signed-files")
        self.assertEqual(receipt["driver_version"], "100.6.101.58474")
        self.assertEqual(receipt["files"][image.name], package.sha(image))
        self.assertNotEqual(receipt["files"][image.name], old_hash)
        self.assertNotIn(package.RECEIPT, receipt["files"])
        self.assertNotIn("viogpuwddm.cat", receipt["files"])
        self.assertIn(package.RECEIPT, package.source_files(self.inf.read_text()))
        self.assertEqual([x["system_directory"] for x in receipt["system_loaders"]],
                         ["System32", "SysWOW64"])
        self.assertEqual(receipt["loader_probes"], package.LOADER_PROBES)
        for name in receipt["loader_probes"].values():
            self.assertEqual(receipt["files"][name], package.sha(self.driver / name))

    def test_reject_missing_helper_mapping(self):
        del self.manifests["opencl"]["loader_probes"]
        self.save()
        self.reject_unchanged("probe mapping")

    def test_reject_cross_architecture_helper_mapping(self):
        self.manifests["opencl"]["loader_probes"]["arm64"] = package.LOADER_PROBES["x86"]
        self.save()
        self.reject_unchanged("probe mapping")

    def test_reject_uninstalled_helper_role(self):
        self.manifests["opencl"]["files"][package.LOADER_PROBES["x64"]]["role"] = "probe"
        self.save()
        self.reject_unchanged("installed x64 public loader helper")

    def test_reject_absent_helper(self):
        name = package.LOADER_PROBES["x86"]
        (self.cl / name).unlink()
        del self.manifests["opencl"]["files"][name]
        self.save()
        self.reject_unchanged("installed x86 public loader helper")

    def test_finalize_rejects_changed_helper_mapping(self):
        self.assemble()
        path = self.driver / package.RECEIPT
        manifest = json.loads(path.read_text())
        manifest["loader_probes"]["arm64"] = "control_x86.exe"
        path.write_text(json.dumps(manifest))
        with self.assertRaisesRegex(ValueError, "probe mapping"):
            package.finalize(self.driver)

    def test_reject_missing_runtime(self):
        name = "viogpu_gles2_x86.dll"
        (self.gl / name).unlink()
        del self.manifests["opengl"]["files"][name]
        self.save()
        self.reject_unchanged("Missing actual x86")

    def test_reject_changed_input(self):
        (self.cl / "viogpucl_arm64.dll").write_bytes(pe("x64"))
        self.reject_unchanged("Changed opencl input")

    def test_reject_wrong_architecture_even_with_matching_hash(self):
        name = "viogpucl_x86.dll"
        (self.cl / name).write_bytes(pe("arm64"))
        self.manifests["opencl"]["files"][name]["sha256"] = package.sha(self.cl / name)
        self.save()
        self.reject_unchanged("PE machine mismatch")

    def test_reject_missing_manifest_file(self):
        (self.cl / "OpenCL32.dll").unlink()
        self.reject_unchanged("cover every file")

    def test_reject_untracked_file(self):
        (self.gl / "stale.dll").write_bytes(pe("x64"))
        self.reject_unchanged("cover every file")

    def test_reject_subdirectory(self):
        (self.gl / "arm64").mkdir()
        self.reject_unchanged("no subdirectories")

    def test_reject_wrong_source(self):
        self.manifests["opengl"]["sources"]["mesa"] = "c" * 40
        self.save()
        self.reject_unchanged("source mismatch")

    def test_reject_wrong_family(self):
        self.manifests["opengl"]["family"] = "opencl"
        self.save()
        self.reject_unchanged("schema/family")

    def test_reject_role_hiding_required_runtime(self):
        self.manifests["opengl"]["files"]["viogpu_egl_arm64.dll"]["role"] = "probe"
        self.save()
        self.reject_unchanged("Missing actual arm64")

    def test_reject_cross_family_collision(self):
        self.add("opengl", "collision.dll", "x64", "runtime")
        self.add("opencl", "collision.dll", "x64", "runtime")
        self.save()
        self.reject_unchanged("Cross-family/driver collision")

    def test_reject_case_insensitive_driver_collision(self):
        self.add("opencl", "VIOGPUD3D.dll", "x64", "runtime")
        self.save()
        self.reject_unchanged("Cross-family/driver collision")

    def test_reject_absolute_vulkan_reference(self):
        self.add("opengl", "turnip.json", "data", "data", json.dumps({
            "file_format_version": "1.0.0", "ICD": {"library_path": "C:\\old\\viogpuopengl.dll"}
        }).encode())
        self.save()
        self.reject_unchanged("same-directory proxy")

    def test_reject_legacy_destination(self):
        self.inf.write_text(self.inf.read_text().replace("DefaultDestDir = 13", "DefaultDestDir = 12"))
        self.reject_unchanged("DIRID13")

    def test_reject_catalog_already_present(self):
        (self.driver / "viogpuwddm.cat").write_bytes(b"existing catalog")
        self.reject_unchanged("after catalog creation")

    def test_reject_second_assembly(self):
        self.assemble()
        self.reject_unchanged("inventory already exists")

    def test_finalize_rejects_late_inf_changes(self):
        self.assemble()
        self.inf.write_text(self.inf.read_text().replace("OpenCLDriverName", "WrongDriverName"))
        with self.assertRaisesRegex(ValueError, "INF changed"):
            package.finalize(self.driver)

    def test_finalize_rejects_uncataloged_extra_file(self):
        self.assemble()
        (self.driver / "extra.dll").write_bytes(pe("arm64"))
        with self.assertRaisesRegex(ValueError, "exactly the flat"):
            package.finalize(self.driver)

    def test_finalize_rejects_missing_file(self):
        self.assemble()
        (self.driver / "viogpu_egl_x86.dll").unlink()
        with self.assertRaisesRegex(ValueError, "exactly the flat"):
            package.finalize(self.driver)

    def test_finalize_rejects_rerun(self):
        self.assemble()
        package.finalize(self.driver)
        with self.assertRaisesRegex(ValueError, "before-signing inventory"):
            package.finalize(self.driver)

    def test_copyfiles_rejects_rename(self):
        self.assemble()
        text = self.inf.read_text().replace("viogpud3d.dll,,,2", "viogpud3d.dll,other.dll,,2")
        with self.assertRaisesRegex(ValueError, "Renamed/non-flat"):
            package.source_files(text)

    def test_windows_unsafe_names(self):
        for name in ("../escape.dll", "x64\\foo.dll", "file.dll:stream", "CON.dll", "a.", "a,b.dll"):
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, "Invalid flat"):
                package.flat_name(name)


if __name__ == "__main__":
    unittest.main()
