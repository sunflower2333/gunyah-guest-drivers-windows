#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Stage verified flat API runtimes into the display INF before PE/catalog signing.

This is build-time staging, never a tool for modifying an installed DriverStore.
Load/ABI/GPU verification is performed separately by the corresponding CI jobs.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct

ARCHES = ("arm64", "x64", "x86")
MACHINES = {"arm64": 0xAA64, "arm64x": 0xAA64, "x64": 0x8664, "x86": 0x14C}
DRIVER_ROLES = {"runtime", "icd", "compiler", "data", "installer-helper"}
ROLES = DRIVER_ROLES | {"system-loader", "probe"}
LOADER_PROBES = {arch: f"opencl-loader-check-{arch}.exe" for arch in ARCHES}
MANIFEST = "flat-runtime.json"
RECEIPT = "viogpu-flat-package.json"
REGISTRATION = {
    "OpenGLDriverName": ("opengl", "viogpuopengl.dll", "arm64x", "0x00010000"),
    "OpenGLDriverNameWow": ("opengl", "viogpuopengl_x86.dll", "x86", "0x00010000"),
    "VulkanDriverName": ("opengl", "turnip.json", "data", "0x00000000"),
    "VulkanDriverNameWow": ("opengl", "turnip-wow.json", "data", "0x00000000"),
    "OpenCLDriverName": ("opencl", "viogpucl.dll", "arm64x", "0x00000000"),
    "OpenCLDriverNameWow": ("opencl", "viogpucl_x86.dll", "x86", "0x00000000"),
}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def flat_name(name):
    require(isinstance(name, str) and len(name) <= 127 and
            re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]*", name) and
            not name.endswith(".") and
            name.split(".")[0].upper() not in
            {"CON", "PRN", "AUX", "NUL", *(f"COM{i}" for i in range(10)),
             *(f"LPT{i}" for i in range(10))}, f"Invalid flat Windows filename: {name}")


def pe_machine(path):
    data = path.read_bytes()
    require(len(data) >= 64 and data[:2] == b"MZ", f"Invalid DOS header: {path}")
    offset = struct.unpack_from("<I", data, 0x3C)[0]
    require(64 <= offset <= len(data) - 24 and data[offset:offset+4] == b"PE\0\0",
            f"Invalid PE header: {path}")
    return struct.unpack_from("<H", data, offset+4)[0]


def read_manifest(root, family, expected_sources):
    require(root.is_dir() and not root.is_symlink(), f"Invalid {family} input directory")
    paths = list(root.iterdir())
    require(all(p.is_file() and not p.is_symlink() for p in paths),
            f"{family} runtime must be flat regular files, no subdirectories/links")
    data = json.loads((root / MANIFEST).read_text(encoding="utf-8-sig"))
    require(data.get("schema") == 1 and data.get("family") == family,
            f"Wrong {family} manifest schema/family")
    require(isinstance(data.get("sources"), dict), f"Missing {family} source identity")
    for key, value in expected_sources.items():
        require(re.fullmatch(r"[a-f0-9]{40}", value) and data["sources"].get(key) == value,
                f"{family} source mismatch: {key}")
    files = data.get("files")
    require(isinstance(files, dict) and files, f"Missing {family} file inventory")
    require({p.name for p in paths} == set(files) | {MANIFEST},
            f"{family} manifest must cover every file except itself")
    seen = set()
    for name, entry in files.items():
        flat_name(name)
        require(name.casefold() not in seen, f"{family} case-insensitive collision: {name}")
        seen.add(name.casefold())
        require(isinstance(entry, dict) and entry.get("role") in ROLES,
                f"Unknown {family} role for {name}")
        require(isinstance(entry.get("sha256"), str) and
                re.fullmatch(r"[a-fA-F0-9]{64}", entry["sha256"]) and
                sha(root / name) == entry["sha256"].lower(), f"Changed {family} input: {name}")
        machine = entry.get("machine")
        if Path(name).suffix.lower() in (".dll", ".exe"):
            require(machine in MACHINES and pe_machine(root / name) == MACHINES[machine],
                    f"{family} PE machine mismatch: {name}")
        else:
            require(machine == "data", f"Non-PE file has an executable machine: {name}")
    return data


def check_required(manifests):
    require(manifests["opencl"].get("loader_probes") == LOADER_PROBES,
            "Missing/wrong public loader probe mapping")
    for arch, name in LOADER_PROBES.items():
        entry = manifests["opencl"]["files"].get(name, {})
        require(entry.get("machine") == arch and entry.get("role") == "installer-helper",
                f"Missing/wrong installed {arch} public loader helper: {name}")
    for _, (family, name, machine, _) in REGISTRATION.items():
        entry = manifests[family]["files"].get(name, {})
        require(entry.get("machine") == machine and entry.get("role") in DRIVER_ROLES,
                f"Missing/wrong registered {family} entrypoint: {name}")
    for arch in ARCHES:
        for stem in ("gl", "egl", "gles1", "gles2", "gl_vk", "gl_loader"):
            name = f"viogpu_{stem}_{arch}.dll"
            entry = manifests["opengl"]["files"].get(name, {})
            require(entry.get("machine") == arch and entry.get("role") == "runtime",
                    f"Missing actual {arch} GL/GLES/Vulkan runtime: {name}")
        for name in (f"viogpucl_{arch}.dll", f"viogpucl_vk_{arch}.dll"):
            entry = manifests["opencl"]["files"].get(name, {})
            require(entry.get("machine") == arch and entry.get("role") in ("runtime", "icd"),
                    f"Missing actual {arch} OpenCL runtime: {name}")
    compiler = manifests["opencl"]["files"].get("viogpu_clspv_x64.exe", {})
    require(compiler.get("machine") == "x64" and compiler.get("role") == "compiler",
            "Missing shared flat OpenCL compiler")
    for name, machine in (("OpenCL.dll", "arm64x"), ("OpenCL32.dll", "x86")):
        loader = manifests["opencl"]["files"].get(name, {})
        require(loader.get("machine") == machine and loader.get("role") == "system-loader",
                f"Missing public loader for unified installer: {name}")


def section_body(text, section):
    pattern = r"(?im)^\[" + re.escape(section) + r"\][ \t]*\r?\n(?P<body>.*?)(?=^\[|\Z)"
    matches = list(re.finditer(pattern, text, re.DOTALL))
    require(len(matches) == 1, f"Expected exactly one INF section: {section}")
    return matches[0]


def append_section(text, section, lines):
    match = section_body(text, section)
    end = match.end("body")
    return text[:end].rstrip() + "\n" + "\n".join(lines) + "\n\n" + text[end:]


def replace_directive(text, section, old, new):
    match = section_body(text, section)
    body = match.group("body")
    require(body.splitlines().count(old) == 1, f"Unexpected {section} directive: {old}")
    body = body.replace(old, new, 1)
    return text[:match.start("body")] + body + text[match.end("body"):]


def compose_inf(text, files):
    require("VioGpuWddm_Api" not in text, "INF API inventory already generated")
    require(not re.search(r"(?i)OpenGLDriverName|OpenCLDriverName|VulkanDriverName", text),
            "INF already contains API registrations")
    destinations = section_body(text, "DestinationDirs").group("body")
    require(re.search(r"(?im)^DefaultDestDir\s*=\s*13\s*$", destinations),
            "Flat API files must run from DriverStore DIRID13")
    require(not re.search(r"(?im)^VioGpuWddm_(CopyFiles|ApiFiles)\s*=", destinations),
            "Unexpected per-list destination override")
    existing = section_body(text, "SourceDisksFiles").group("body")
    existing_names = {line.split("=", 1)[0].strip().casefold()
                      for line in existing.splitlines() if "=" in line and not line.lstrip().startswith(";")}
    require(not existing_names.intersection(n.casefold() for n in files),
            "API name collides with existing INF source entry")
    text = append_section(text, "SourceDisksFiles", [f"{name} = 1,," for name in files])
    text = replace_directive(text, "VioGpuWddm_Install.NT",
        "CopyFiles = VioGpuWddm_CopyFiles",
        "CopyFiles = VioGpuWddm_CopyFiles, VioGpuWddm_ApiFiles")
    text = replace_directive(text, "VioGpuWddm_Install.NT",
        "AddReg = VioGpuWddm_DeviceSettings",
        "AddReg = VioGpuWddm_DeviceSettings, VioGpuWddm_ApiSettings")
    text += "\n[VioGpuWddm_ApiFiles]\n" + "\n".join(files) + "\n"
    text += "\n[VioGpuWddm_ApiSettings]\n"
    for key, (_, filename, _, flags) in REGISTRATION.items():
        text += f'HKR,,{key},{flags},"%13%\\{filename}"\n'
    for suffix in ("", "Wow"):
        text += f"HKR,,OpenGLVersion{suffix},0x00010001,1\n"
        text += f"HKR,,OpenGLFlags{suffix},0x00010001,1\n"
    return text


def assemble(driver, gl, cl, mesa, clvk):
    require(driver.is_dir() and not driver.is_symlink(), "Missing staged driver directory")
    require(all(p.is_file() and not p.is_symlink() for p in driver.iterdir()),
            "Staged driver must contain flat regular files")
    require(not any(p.suffix.lower() == ".cat" for p in driver.iterdir()),
            "Cannot modify a driver directory after catalog creation")
    roots = {"opengl": gl, "opencl": cl}
    manifests = {"opengl": read_manifest(gl, "opengl", {"mesa": mesa}),
                 "opencl": read_manifest(cl, "opencl", {"clvk": clvk})}
    check_required(manifests)
    for name, proxy in (("turnip.json", "viogpuopengl.dll"),
                        ("turnip-wow.json", "viogpuopengl_x86.dll")):
        data = json.loads((gl / name).read_text(encoding="utf-8-sig"))
        require(data.get("file_format_version") == "1.0.0" and
                data.get("ICD", {}).get("library_path") in (proxy, ".\\" + proxy, "./" + proxy),
                f"Vulkan manifest must select its same-directory proxy: {name}")
    existing = {p.name.casefold() for p in driver.iterdir()}
    require(RECEIPT.casefold() not in existing, "Driver inventory already exists")
    files = {}
    public_loaders = {}
    all_names = set(existing) | {RECEIPT.casefold()}
    for family, manifest in manifests.items():
        for name, entry in manifest["files"].items():
            if entry["role"] == "probe":
                continue
            require(name.casefold() not in all_names, f"Cross-family/driver collision: {name}")
            all_names.add(name.casefold())
            if entry["role"] == "system-loader":
                public_loaders[name] = {**entry, "family": family}
            require(entry["role"] in DRIVER_ROLES | {"system-loader"},
                    f"Unsupported driver file: {name}")
            files[name] = {**entry, "family": family}
    inf = driver / "viogpuwddm.inf"
    original = inf.read_text(encoding="utf-8-sig")
    updated = compose_inf(original, sorted(set(files) | {RECEIPT}, key=str.casefold))
    # All inputs and the complete resulting INF are checked before any mutation.
    # Preserve exact filenames: DIRID13 forbids CopyFiles renaming.
    for name, entry in files.items():
        source = roots[entry["family"]] / name
        require(sha(source) == entry["sha256"].lower(), f"Input changed during assembly: {name}")
    for name, entry in files.items():
        with (roots[entry["family"]] / name).open("rb") as source, (driver / name).open("xb") as dest:
            shutil.copyfileobj(source, dest)
        require(sha(driver / name) == entry["sha256"].lower(), f"Staged hash mismatch: {name}")
    inf.write_text(updated, encoding="utf-8")
    receipt = {"schema": 1, "layout": "flat-driverstore", "phase": "before-signing",
               "sources": {family: data["sources"] for family, data in manifests.items()},
               "inf_sha256": sha(inf), "api_files_before_signing": files,
               "public_loaders": public_loaders,
               "loader_probes": manifests["opencl"]["loader_probes"],
               "registration": {key: value[1] for key, value in REGISTRATION.items()}}
    (driver / RECEIPT).write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    return receipt


def source_files(inf_text):
    """Return actual copied filenames, rejecting renames/subdirectories."""
    source = section_body(inf_text, "SourceDisksFiles").group("body")
    names = []
    for line in source.splitlines():
        line = line.strip()
        if not line or line.startswith(";"):
            continue
        match = re.fullmatch(r"([A-Za-z0-9_.-]+)\s*=\s*1\s*,\s*,\s*", line)
        require(match is not None, f"Unexpected/non-flat INF source entry: {line}")
        name = match[1]
        flat_name(name)
        require(name.casefold() not in {n.casefold() for n in names}, "Duplicate INF source file")
        names.append(name)
    copied = []
    for section in ("VioGpuWddm_CopyFiles", "VioGpuWddm_ApiFiles"):
        for line in section_body(inf_text, section).group("body").splitlines():
            line = line.strip()
            if not line or line.startswith(";"):
                continue
            match = re.fullmatch(r"([A-Za-z0-9_.-]+)(?:,,,2)?", line)
            require(match is not None, f"Renamed/non-flat CopyFiles entry: {line}")
            copied.append(match[1])
    require(sorted(n.casefold() for n in names) == sorted(n.casefold() for n in copied),
            "INF source and actual copy inventories differ")
    return names


def finalize(driver):
    """Inventory signed PEs immediately before Inf2Cat; signature is checked by CI/install."""
    manifest_path = driver / RECEIPT
    require(driver.is_dir() and not driver.is_symlink(), "Invalid driver directory")
    inventory = json.loads(manifest_path.read_text(encoding="utf-8-sig"))
    require(inventory.get("schema") == 1 and inventory.get("layout") == "flat-driverstore" and
            inventory.get("phase") == "before-signing", "Expected fresh before-signing inventory")
    require(inventory.get("loader_probes") == LOADER_PROBES,
            "Missing/wrong public loader probe mapping")
    for arch, name in LOADER_PROBES.items():
        entry = inventory["api_files_before_signing"].get(name, {})
        require(entry.get("machine") == arch and entry.get("role") == "installer-helper",
                f"Missing/wrong installed {arch} public loader helper: {name}")
    inf_name = "viogpuwddm.inf"
    inf = driver / inf_name
    require(sha(inf) == inventory["inf_sha256"], "INF changed after flat composition")
    text = inf.read_text(encoding="utf-8-sig")
    names = source_files(text)
    require(set(names) == set(inventory["api_files_before_signing"]) |
            {"viogpuwddm.sys", "viogpud3d.dll", RECEIPT}, "Unexpected copied file set")
    paths = list(driver.iterdir())
    require(all(p.is_file() and not p.is_symlink() for p in paths) and
            {p.name for p in paths} == set(names) | {inf_name},
            "Final driver must contain exactly the flat INF payload and manifest before cataloging")
    version = re.search(r"(?im)^DriverVer\s*=\s*[^,\r\n]+,\s*([0-9.]+)\s*$", text)
    require(version is not None, "Missing concrete driver version")
    require('CatalogFile = viogpuwddm.cat' in text, "Unexpected driver catalog")
    for key, (_, filename, _, flags) in REGISTRATION.items():
        require(f'HKR,,{key},{flags},"%13%\\{filename}"' in text,
                f"Missing device-scoped registration: {key}")
    files = {name: sha(driver / name) for name in sorted((set(names) | {inf_name}) - {RECEIPT})}
    loaders = []
    for source, directory, arch in (("OpenCL.dll", "System32", "arm64x"),
                                     ("OpenCL32.dll", "SysWOW64", "x86")):
        require(inventory["public_loaders"][source]["machine"] == arch,
                f"Invalid public loader architecture: {source}")
        loaders.append({"source": source, "system_directory": directory,
                        "name": "OpenCL.dll", "sha256": files[source], "arch": arch})
    result = {"schema": 1, "layout": "flat-driverstore", "phase": "signed-files",
              "driver_version": version[1], "hardware_ids": [r"PCI\VEN_1AF4&DEV_1050"],
              "inf": inf_name, "cat": "viogpuwddm.cat", "files": files,
              "sources": inventory["sources"], "system_loaders": loaders,
              "loader_probes": inventory["loader_probes"],
              "registration": inventory["registration"]}
    # No circular catalog/manifest hashes: the INF copies this manifest and
    # Inf2Cat covers it. A separate final receipt can hash the signed catalog.
    manifest_path.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--driver", type=Path, required=True)
    parser.add_argument("--finalize", action="store_true")
    parser.add_argument("--gl", type=Path)
    parser.add_argument("--cl", type=Path)
    parser.add_argument("--mesa")
    parser.add_argument("--clvk")
    args = parser.parse_args()
    if args.finalize:
        receipt = finalize(args.driver)
        print(f"PASS final inventory of {len(receipt['files'])} driver files; catalog signing pending")
        return
    parser.error("--gl --cl --mesa --clvk required for staging") if any(
        value is None for value in (args.gl, args.cl, args.mesa, args.clvk)) else None
    receipt = assemble(args.driver, args.gl, args.cl, args.mesa, args.clvk)
    print(f"PASS staged {len(receipt['api_files_before_signing'])} flat API files in display INF")
    print("PENDING PE/catalog signing, InfVerif, public-loader install, ABI and GPU tests")


if __name__ == "__main__":
    main()
