#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Check signed sidecar bindings and emit a reviewable joint package receipt.

PE, signer, catalog and loader checks run in the preceding packaging steps.
This receipt checks their final file identities; it is not GPU runtime proof.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def git_value(*args):
    return subprocess.check_output(["git", *args], text=True).strip()


def sidecar(output, name, source_name, parent, kmd, umd):
    payload = output / name / "payload"
    binding_path = payload / "package-binding.json"
    binding = read_json(binding_path)
    require(binding["parent_commit"] == parent, f"{name}: wrong parent")
    require(binding["kmd_sha256"].lower() == kmd, f"{name}: wrong signed KMD")
    require(binding["d3d_umd_sha256"].lower() == umd, f"{name}: wrong signed UMD")
    actual = {path.relative_to(payload).as_posix(): digest(path)
              for path in payload.rglob("*")
              if path.is_file() and path != binding_path}
    expected = {path: sha.lower() for path, sha in binding["files_after_signing"].items()}
    require(actual == expected, f"{name}: signed payload differs from binding")
    return {
        "binding_sha256": digest(binding_path),
        "catalog_sha256": digest(output / name / f"{name}.cat"),
        "source_manifest_sha256": digest(payload / source_name),
        "bound_files": len(actual),
    }, read_json(payload / source_name), actual


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--mesa", required=True)
    parser.add_argument("--mesa-run", required=True, type=int)
    parser.add_argument("--clvk", required=True)
    parser.add_argument("--version", required=True)
    args = parser.parse_args()
    parent = git_value("rev-parse", "HEAD")
    require(parent == os.environ.get("GITHUB_SHA", parent), "CI parent mismatch")
    require(git_value("rev-parse", "HEAD:external/mesa") == args.mesa,
            "Committed Mesa gitlink mismatch")
    driver = args.output / "drivers" / "viogpu"
    inf = driver / "viogpuwddm.inf"
    require(re.search(r"^DriverVer\s*=\s*[^,\r\n]+,\s*" + re.escape(args.version) + r"\s*$",
                      inf.read_text(encoding="utf-8-sig"), re.MULTILINE),
            "Packaged driver version mismatch")
    kmd = digest(driver / "viogpuwddm.sys")
    umd = digest(driver / "viogpud3d.dll")
    gl, gl_source, gl_files = sidecar(args.output, "opengl", "source-identity.json",
                                     parent, kmd, umd)
    cl, cl_source, cl_files = sidecar(args.output, "opencl", "sources.json",
                                     parent, kmd, umd)
    require(gl_source["mesa_commit"] == args.mesa, "GL Mesa source mismatch")
    require(gl_source["mesa_run"] == args.mesa_run, "GL Mesa run mismatch")
    require(cl_source["clvk"] == args.clvk, "CL runtime source mismatch")
    require(cl_source["clvk_runtime_ci"] == 34613435673, "CL runtime run changed")
    require(cl_source["compiler_original_sha256"] ==
            "79e236af8febd67fd02adfd93f81295c87e868e9fd861f71d03d1057e6be1f9d",
            "CL compiler changed")
    dlls = ("libgallium_wgl.dll", "libEGL.dll", "libGLESv1_CM.dll", "libGLESv2.dll",
            "vulkan_freedreno.dll", "vulkan-1.dll", "z-1.dll")
    mesa_files = {f"{arch}/{dll}" for arch in ("arm64", "x64", "x86") for dll in dlls}
    require(set(gl_source["files"]) == mesa_files, "Incomplete Mesa architecture identities")
    require(mesa_files <= gl_files.keys(), "Missing signed Mesa architecture binaries")
    for arch in ("arm64", "x64", "x86"):
        require(f"{arch}/viogpucl.dll" in cl_files, f"Missing signed {arch} CL runtime")
    receipt = {
        "schema": 1,
        "validation": "joint package identities; GPU runtime validation remains separate",
        "parent_commit": parent,
        "driver_version": args.version,
        "mesa_commit": args.mesa,
        "mesa_run": args.mesa_run,
        "clvk_commit": args.clvk,
        "clvk_runtime_ci": cl_source["clvk_runtime_ci"],
        "kmd_sha256": kmd,
        "d3d_umd_sha256": umd,
        "inf_sha256": digest(inf),
        "driver_catalog_sha256": digest(driver / "viogpuwddm.cat"),
        "sidecars": {"opengl": gl, "opencl": cl},
        "mesa_files_after_signing": {path: gl_files[path] for path in sorted(mesa_files)},
        "clvk_files_after_signing": {arch: cl_files[f"{arch}/viogpucl.dll"]
                                     for arch in ("arm64", "x64", "x86")},
    }
    (args.output / "joint-package-receipt.json").write_text(
        json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    print("PASS exact jointly signed package binding and all three Mesa/CL architectures")
    print("JOINT_PACKAGE_RECEIPT=" + json.dumps(receipt, sort_keys=True))


if __name__ == "__main__":
    main()
