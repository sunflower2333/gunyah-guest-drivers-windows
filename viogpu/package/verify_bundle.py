#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Verify final flat identities after Windows signature/catalog/ABI checks."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess

from flat_package import CANDIDATE_SOURCES, CANDIDATE_UMDS, D3D10_FILES, D3D_REGISTRATION
from flat_package import d3d_registration_line
from flat_package import LOADER_PROBES, MACHINES, RECEIPT, REGISTRATION
from flat_package import flat_name, pe_machine, require, sha, source_files

# D3D10 UMDs follow the checked-in Mesa gitlink. This is independent from the
# older OpenGL ICD artifact pin verified by the reusable OpenGL workflow.
D3D10_MESA = "a304f0ff445bfd78cd82c9dee82632b7b766dae7"

INSTALLER_FILES = (
    "INSTALL.cmd", "install-drivers.ps1", "pvmpower-devnode.ps1",
    "viogpu-unified-install.ps1", "viogpu-install-state.psm1",
    "viogpu-install-native.cs", "viogpu-api-registration.psm1",
    "viogpu-install-certificates.psm1", "DroidVM_Test.cer",
)
DEBUG_FILES = {"viogpuwddm.pdb", "viogpuwddm.map", "viogpud3d.pdb",
               "viogpud3dx.pdb", "viogpud3d_x64.pdb", "viogpud3d_x86.pdb"}


def verify(output, parent, mesa, mesa_run, clvk, clvk_run, version):
    driver = output / "drivers/viogpu"
    manifest = json.loads((driver / RECEIPT).read_text(encoding="utf-8-sig"))
    require(manifest.get("schema") == 1 and manifest.get("phase") == "signed-files" and
            manifest.get("layout") == "flat-driverstore", "Wrong final manifest format")
    require(manifest["driver_version"] == version and
            manifest["hardware_ids"] == [r"PCI\VEN_1AF4&DEV_1050"] and
            manifest["inf"] == "viogpuwddm.inf" and manifest["cat"] == "viogpuwddm.cat",
            "Wrong final driver identity")
    gl, cl = manifest["sources"]["opengl"], manifest["sources"]["opencl"]
    d3d10 = manifest["sources"]["d3d10"]
    require(gl["parent"] == cl["parent"] == d3d10["parent"] == parent, "Mixed producer parent revisions")
    require(d3d10["mesa"] == D3D10_MESA, "Wrong D3D10 Mesa source for the x64/x86 UMDs")
    require(gl["mesa"] == mesa and gl["mesa_run"] == mesa_run, "Wrong OpenGL Mesa source/run")
    require(cl["clvk"] == clvk and cl["clvk_runtime_ci"] == clvk_run, "Wrong CLVK source/run")
    require(cl["compiler_original_sha256"] ==
            "79e236af8febd67fd02adfd93f81295c87e868e9fd861f71d03d1057e6be1f9d",
            "Wrong OpenCL compiler source identity")
    require(manifest.get("candidate_sources") == CANDIDATE_SOURCES,
            "Wrong DXVK/VKD3D candidate source pins")
    require(manifest.get("candidate_activation") == "unregistered-candidate",
            "DXVK/VKD3D candidates must remain unregistered")
    candidates = manifest.get("candidate_umds")
    require(isinstance(candidates, dict) and set(candidates) == set(CANDIDATE_UMDS),
            "Wrong DXVK/VKD3D candidate inventory")
    require(manifest["registration"] == {k: v[1] for k, v in REGISTRATION.items()},
            "Wrong API registration mapping")
    registered = {name.casefold() for name in manifest["registration"].values()}
    for name, (family, machine) in CANDIDATE_UMDS.items():
        entry = candidates[name]
        require(entry.get("family") == family and entry.get("machine") == machine and
                entry.get("role") == "candidate-runtime", f"Wrong candidate metadata: {name}")
        require(name.casefold() not in registered, f"Candidate UMD became registered: {name}")
        require(pe_machine(driver / name) == MACHINES[machine], f"Wrong candidate PE architecture: {name}")
    require(manifest["loader_probes"] == LOADER_PROBES, "Wrong loader helper mapping")
    require(manifest.get("d3d_registration") == D3D_REGISTRATION, "Wrong D3D UMD registration mapping")
    inf = (driver / manifest["inf"]).read_text(encoding="utf-8-sig")
    require(re.search(r"(?m)^DriverVer\s*=\s*[^,\r\n]+,\s*" + re.escape(version) + r"\s*$", inf),
            "INF version mismatch")
    names = set(source_files(inf))
    require(set(manifest["files"]) == (names - {RECEIPT}) | {manifest["inf"]},
            "Final manifest and INF copy inventory differ")
    require(set(LOADER_PROBES.values()) <= names, "Missing cataloged loader helpers")
    require(set(CANDIDATE_UMDS) <= names, "Missing cataloged DXVK/VKD3D candidates")
    for key, (_, name, _, flags) in REGISTRATION.items():
        require(f'HKR,,{key},{flags},"%13%\\{name}"' in inf, f"Wrong INF API value: {key}")
    for key, name in D3D_REGISTRATION.items():
        require(inf.splitlines().count(d3d_registration_line(key, name)) == 1, f"Wrong INF D3D UMD value: {key}")
    require(set(D3D10_FILES) <= names, "Missing cataloged D3D10 user-mode drivers")
    paths = list(driver.iterdir())
    require(all(p.is_file() and not p.is_symlink() and p.stat().st_size for p in paths) and
            {p.name for p in paths} == names | {manifest["inf"], manifest["cat"]} | DEBUG_FILES,
            "Unexpected/missing flat payload or debug files")
    for name, digest in manifest["files"].items():
        flat_name(name)
        require(sha(driver / name) == digest, f"Signed file changed: {name}")
    for arch, name in LOADER_PROBES.items():
        require(pe_machine(driver / name) == MACHINES[arch], f"Wrong helper PE architecture: {name}")
    expected_loaders = [{"source": source, "system_directory": directory, "name": "OpenCL.dll",
                         "sha256": manifest["files"][source], "arch": arch}
                        for source, directory, arch in (("OpenCL.dll", "System32", "arm64x"),
                                                        ("OpenCL32.dll", "SysWOW64", "x86"))]
    require(manifest["system_loaders"] == expected_loaders, "Wrong public loader mapping")
    for name in INSTALLER_FILES:
        require((output / name).is_file() and not (output / name).is_symlink() and
                (output / name).stat().st_size, f"Missing installer root file: {name}")
    require(not any((output / name).exists() for name in ("opengl", "opencl", "DroidVM_Test.pfx")),
            "Obsolete sidecar or private signing key in final bundle")
    return {"schema": 1, "layout": "flat-driverstore", "parent_commit": parent,
            "driver_version": version, "sources": manifest["sources"],
            "d3d10_mesa": D3D10_MESA,
            "d3d_registration": manifest["d3d_registration"],
            "candidate_sources": manifest["candidate_sources"],
            "candidate_activation": manifest["candidate_activation"],
            "candidate_umds": manifest["candidate_umds"],
            "validation": "signed catalog and architecture loading; actual GPU acceptance remains separate",
            "gpu_files": {p.name: sha(p) for p in sorted(paths)},
            "installer_files": {name: sha(output / name) for name in INSTALLER_FILES}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    for option in ("mesa", "clvk", "version"):
        parser.add_argument("--" + option, required=True)
    for option in ("mesa-run", "clvk-run"):
        parser.add_argument("--" + option, required=True, type=int)
    args = parser.parse_args()

    def git(*arguments):
        return subprocess.check_output(["git", *arguments], text=True).strip()

    parent = git("rev-parse", "HEAD")
    require(parent == os.environ.get("GITHUB_SHA", parent), "CI parent mismatch")
    d3d10_mesa = git("rev-parse", "HEAD:external/mesa")
    require(d3d10_mesa == D3D10_MESA, "Committed D3D10 Mesa gitlink mismatch")
    receipt = verify(args.output, parent, args.mesa, args.mesa_run, args.clvk, args.clvk_run, args.version)
    require(receipt["d3d10_mesa"] == d3d10_mesa, "D3D10 Mesa receipt mismatch")
    (args.output / "joint-package-receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print(f"PASS unified flat bundle: {len(receipt['gpu_files'])} GPU files, "
          f"{len(receipt['installer_files'])} installer files, version {args.version}")


if __name__ == "__main__":
    main()
