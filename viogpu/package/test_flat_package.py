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
D3D10_MESA = "d" * 40


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
        self.driver, self.gl, self.cl, self.d3d10, self.candidates = (
            self.root / name for name in ("driver", "gl", "cl", "d3d10", "candidates"))
        for directory in (self.driver, self.gl, self.cl, self.d3d10, self.candidates):
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
            "d3d10": {"schema": 1, "family": "d3d10", "sources": {"mesa": D3D10_MESA}, "files": {}},
        }
        for name, (machine, role) in package.D3D10_FILES.items():
            self.add("d3d10", name, machine, role)
        for arch in package.ARCHES:
            self.add("d3d10", f"d3d-umd-probe-{arch}.exe", arch, "probe")
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
        self.save_candidates()

    def add(self, family, name, machine, role, content=None):
        directory = {"opengl": self.gl, "opencl": self.cl, "d3d10": self.d3d10}[family]
        data = content if content is not None else pe(machine)
        (directory / name).write_bytes(data)
        self.manifests[family]["files"][name] = {
            "sha256": hashlib.sha256(data).hexdigest(), "machine": machine, "role": role}

    def save(self):
        for family, path in (("opengl", self.gl), ("opencl", self.cl), ("d3d10", self.d3d10)):
            (path / package.MANIFEST).write_text(json.dumps(self.manifests[family]))

    def save_candidates(self):
        files = {}
        for name, (family, machine) in package.CANDIDATE_UMDS.items():
            path = self.candidates / name
            if not path.exists():
                path.write_bytes(pe(machine))
            files[name] = {
                "family": family,
                "machine": machine,
                "role": "candidate-runtime",
                "sha256": package.sha(path),
            }
            if name in package.DXVK_VULKAN_LOADERS:
                files[name]["admission"] = package.CLOSED_ADMISSION + "StreamOutput"
                files[name]["vulkan_loader"] = package.DXVK_VULKAN_LOADERS[name]
        (self.candidates / package.CANDIDATE_MANIFEST).write_text(json.dumps({
            "schema": 1,
            "activation": "unregistered-candidate",
            "sources": dict(package.CANDIDATE_SOURCES),
            "files": files,
        }))

    def assemble(self):
        return package.assemble(self.driver, self.gl, self.cl, self.d3d10, MESA, CLVK, D3D10_MESA,
                                self.candidates)

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
        self.assertTrue(set(package.CANDIDATE_UMDS) <= set(copied))
        self.assertEqual(receipt["candidate_sources"], package.CANDIDATE_SOURCES)
        self.assertEqual(receipt["candidate_activation"], "unregistered-candidate")
        self.assertTrue(all(p.is_file() for p in self.driver.iterdir()))
        for key, (_, name, _, flags) in package.REGISTRATION.items():
            self.assertIn(f'HKR,,{key},{flags},"%13%\\{name}"', text)
        registered = set(receipt["registration"].values())
        self.assertFalse(registered.intersection(package.CANDIDATE_UMDS))
        self.assertNotIn("Program Files", text)
        self.assertNotIn("SOFTWARE\\Khronos", text)

    def test_d3d_umd_registration_selects_arm64x_entry_and_wow_umd(self):
        receipt = self.assemble()
        text = self.inf.read_text()
        copied = package.source_files(text)
        self.assertTrue(set(package.D3D10_FILES) <= set(copied))
        self.assertIn("viogpud3d.dll", copied)  # the native Mesa UMD behind the entry
        self.assertFalse({f"d3d-umd-probe-{arch}.exe" for arch in package.ARCHES} & set(copied))
        lines = text.splitlines()
        self.assertEqual(lines.count(package.d3d_registration_line("UserModeDriverName", "viogpud3dx.dll")), 1)
        self.assertEqual(lines.count(package.d3d_registration_line("UserModeDriverNameWow", "viogpud3d_x86.dll")), 1)
        self.assertNotIn(package.NATIVE_UMD_REGISTRATION, text)
        self.assertEqual(receipt["d3d_registration"], package.D3D_REGISTRATION)
        self.assertNotIn("UserModeDriverName", receipt["registration"])
        self.assertEqual(package.finalize(self.driver)["d3d_registration"], package.D3D_REGISTRATION)

    def test_reject_missing_x86_d3d_umd(self):
        name = "viogpud3d_x86.dll"
        (self.d3d10 / name).unlink()
        del self.manifests["d3d10"]["files"][name]
        self.save()
        self.reject_unchanged("Missing actual x86 D3D10")

    def test_reject_native_only_d3d_entry(self):
        name = "viogpud3dx.dll"
        self.manifests["d3d10"]["files"][name]["machine"] = "arm64"
        self.save()
        self.reject_unchanged("Missing actual arm64x D3D10")

    def test_reject_wrong_d3d10_source(self):
        self.manifests["d3d10"]["sources"]["mesa"] = "e" * 40
        self.save()
        self.reject_unchanged("d3d10 source mismatch")

    def test_reject_inf_without_native_umd_registration(self):
        self.inf.write_text(self.inf.read_text().replace('"%13%\\viogpud3d.dll","%13%\\viogpud3d.dll"',
                                                         '"%13%\\other.dll","%13%\\viogpud3d.dll"'))
        self.reject_unchanged("Unexpected VioGpuWddm_DeviceSettings directive")

    def test_finalize_rejects_dropped_wow_registration(self):
        self.assemble()
        text = self.inf.read_text().replace("UserModeDriverNameWow", "UserModeDriverNameOld")
        self.inf.write_text(text)
        path = self.driver / package.RECEIPT
        manifest = json.loads(path.read_text())
        manifest["inf_sha256"] = package.sha(self.inf)
        path.write_text(json.dumps(manifest))
        with self.assertRaisesRegex(ValueError, "D3D UMD registration: UserModeDriverNameWow"):
            package.finalize(self.driver)

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
        self.assertEqual(receipt["candidate_sources"], package.CANDIDATE_SOURCES)
        self.assertEqual(receipt["candidate_activation"], "unregistered-candidate")
        for name in receipt["loader_probes"].values():
            self.assertEqual(receipt["files"][name], package.sha(self.driver / name))
        for name in package.CANDIDATE_UMDS:
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

    def test_reject_candidate_source_pin(self):
        manifest = json.loads((self.candidates / package.CANDIDATE_MANIFEST).read_text())
        manifest["sources"]["dxvk"] = "0" * 40
        (self.candidates / package.CANDIDATE_MANIFEST).write_text(json.dumps(manifest))
        self.reject_unchanged("source pin")

    def test_reject_candidate_wrong_architecture(self):
        name = "viogpudxvk.dll"
        (self.candidates / name).write_bytes(pe("x64"))
        self.save_candidates()
        self.reject_unchanged("Candidate PE machine mismatch")

    def test_reject_missing_candidate(self):
        (self.candidates / "viogpud3d12.dll").unlink()
        self.reject_unchanged("exact flat input")

    def test_reject_candidate_subdirectory(self):
        (self.candidates / "nested").mkdir()
        self.reject_unchanged("no subdirectories")

    def edit_candidate(self, name, **fields):
        path = self.candidates / package.CANDIDATE_MANIFEST
        manifest = json.loads(path.read_text())
        manifest["files"][name].update(fields)
        path.write_text(json.dumps(manifest))

    def test_dxvk_candidate_records_closed_gate_and_private_loader(self):
        receipt = self.assemble()
        for name, arch in (("viogpudxvk.dll", "arm64"), ("viogpudxvk_x64.dll", "x64"),
                           ("viogpudxvk_x86.dll", "x86")):
            entry = receipt["candidate_umds"][name]
            self.assertEqual(entry["machine"], arch)
            self.assertTrue(entry["admission"].startswith("closed; remaining: "))
            self.assertEqual(entry["vulkan_loader"], f"viogpu_gl_loader_{arch}.dll")
        self.assertEqual(receipt["candidate_umds"]["viogpudxvkx.dll"]["machine"], "arm64x")
        text = self.inf.read_text()
        self.assertTrue(set(package.CANDIDATE_UMDS) <= set(package.source_files(text)))
        self.assertNotIn("viogpudxvk", "\n".join(line for line in text.splitlines() if "DriverName" in line))
        final = package.finalize(self.driver)
        self.assertEqual(final["candidate_umds"], receipt["candidate_umds"])

    def test_candidate_d3d_registration_is_receipt_data_only(self):
        receipt = self.assemble()
        self.assertEqual(receipt["candidate_d3d_registration"],
                         {"UserModeDriverName": "viogpudxvkx.dll", "UserModeDriverNameWow": "viogpudxvk_x86.dll"})
        self.assertEqual(receipt["d3d_registration"], package.D3D_REGISTRATION)
        self.assertNotIn("UserModeDriverName", receipt["registration"])
        self.assertEqual(package.finalize(self.driver)["candidate_d3d_registration"],
                         package.CANDIDATE_D3D_REGISTRATION)

    def test_finalize_rejects_candidate_written_into_inf(self):
        self.assemble()
        # The real registrations stay intact; only an extra candidate line is added.
        self.inf.write_text(self.inf.read_text() + "HKR,,UserModeDriverName,%REG_MULTI_SZ%,"
                            '"%13%\\viogpudxvkx.dll"\n')
        path = self.driver / package.RECEIPT
        manifest = json.loads(path.read_text())
        manifest["inf_sha256"] = package.sha(self.inf)
        path.write_text(json.dumps(manifest))
        with self.assertRaisesRegex(ValueError, "Candidate UMD written into the INF"):
            package.finalize(self.driver)

    def test_finalize_rejects_changed_candidate_registration(self):
        self.assemble()
        path = self.driver / package.RECEIPT
        manifest = json.loads(path.read_text())
        manifest["candidate_d3d_registration"]["UserModeDriverName"] = "viogpudxvk.dll"
        path.write_text(json.dumps(manifest))
        with self.assertRaisesRegex(ValueError, "opt-in candidate D3D registration"):
            package.finalize(self.driver)

    def test_reject_missing_x86_dxvk_candidate(self):
        (self.candidates / "viogpudxvk_x86.dll").unlink()
        self.reject_unchanged("exact flat input")

    def test_reject_native_only_dxvk_entry(self):
        (self.candidates / "viogpudxvkx.dll").unlink()
        self.save_candidates()
        manifest = json.loads((self.candidates / package.CANDIDATE_MANIFEST).read_text())
        (self.candidates / "viogpudxvkx.dll").write_bytes(pe("x64"))
        manifest["files"]["viogpudxvkx.dll"]["sha256"] = package.sha(self.candidates / "viogpudxvkx.dll")
        (self.candidates / package.CANDIDATE_MANIFEST).write_text(json.dumps(manifest))
        self.reject_unchanged("Candidate PE machine mismatch")

    def test_reject_dxvk_candidate_with_open_gate(self):
        self.edit_candidate("viogpudxvk.dll", admission="OPEN")
        self.reject_unchanged("closed admission gate")

    def test_reject_dxvk_candidate_without_gate_record(self):
        manifest = json.loads((self.candidates / package.CANDIDATE_MANIFEST).read_text())
        del manifest["files"]["viogpudxvk.dll"]["admission"]
        (self.candidates / package.CANDIDATE_MANIFEST).write_text(json.dumps(manifest))
        self.reject_unchanged("closed admission gate")

    def test_reject_dxvk_candidate_on_public_vulkan_loader(self):
        self.edit_candidate("viogpudxvk.dll", vulkan_loader="vulkan-1.dll")
        self.reject_unchanged("private Vulkan loader")

    def test_finalize_rejects_opened_dxvk_gate(self):
        self.assemble()
        path = self.driver / package.RECEIPT
        manifest = json.loads(path.read_text())
        manifest["candidate_umds"]["viogpudxvk.dll"]["admission"] = "OPEN"
        path.write_text(json.dumps(manifest))
        with self.assertRaisesRegex(ValueError, "closed admission gate"):
            package.finalize(self.driver)

    def test_finalize_rejects_candidate_activation(self):
        self.assemble()
        path = self.driver / package.RECEIPT
        manifest = json.loads(path.read_text())
        manifest["candidate_activation"] = "registered"
        path.write_text(json.dumps(manifest))
        with self.assertRaisesRegex(ValueError, "unregistered candidates"):
            package.finalize(self.driver)

    def test_windows_unsafe_names(self):
        for name in ("../escape.dll", "x64\\foo.dll", "file.dll:stream", "CON.dll", "a.", "a,b.dll"):
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, "Invalid flat"):
                package.flat_name(name)


if __name__ == "__main__":
    unittest.main()
