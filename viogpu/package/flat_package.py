#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Stage verified flat API runtimes into the display INF before PE/catalog signing.

This is build-time staging, never a tool for modifying an installed DriverStore.
Load/ABI/GPU verification is performed separately by the corresponding CI jobs.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess

ARCHES = ("arm64", "x64", "x86")
MACHINES = {"arm64": 0xAA64, "arm64x": 0xAA64, "x64": 0x8664, "x86": 0x14C}
DRIVER_ROLES = {"runtime", "icd", "compiler", "data", "installer-helper", "candidate-runtime"}
ROLES = DRIVER_ROLES | {"system-loader", "probe"}
LOADER_PROBES = {arch: f"opencl-loader-check-{arch}.exe" for arch in ARCHES}
MANIFEST = "flat-runtime.json"
RECEIPT = "viogpu-flat-package.json"
CANDIDATE_MANIFEST = "candidate-sources.json"
CANDIDATE_SOURCES = {
    "dxvk": "a677ab073ac09b1d4eacfabec0e8e10054d58e97",
    "vkd3d": "376e716e4acdf7a9ded138b0099f2ee8a8863f91",
}
CANDIDATE_UMDS = {
    "viogpudxvkx.dll": ("dxvk", "arm64x"),
    "viogpudxvk.dll": ("dxvk", "arm64"),
    "viogpudxvk_x64.dll": ("dxvk", "x64"),
    "viogpudxvk_x86.dll": ("dxvk", "x86"),
    "viogpud3d12.dll": ("vkd3d", "arm64"),
}
# Symbols shipped beside the signed package (not cataloged) for candidates built
# by their own CI job, and the private Vulkan loader each DXVK build resolves.
CANDIDATE_SYMBOLS = {f"{Path(name).stem}.pdb": name for name, (family, _) in CANDIDATE_UMDS.items()
                     if family == "dxvk"}
DXVK_VULKAN_LOADERS = {
    "viogpudxvk.dll": "viogpu_gl_loader_arm64.dll",
    "viogpudxvk_x64.dll": "viogpu_gl_loader_x64.dll",
    "viogpudxvk_x86.dll": "viogpu_gl_loader_x86.dll",
}
CLOSED_ADMISSION = "closed; remaining: "
# Opt-in only. The D3D runtime would load DXVK through these adapter values,
# mirroring D3D_REGISTRATION: the ARM64X entry for native ARM64 and emulated x64
# processes, the x86 UMD for WoW64. Composition never writes them; the receipt
# records them so a deliberate registry trial names exact package files. The
# unified installer neither reads nor applies this key.
CANDIDATE_D3D_REGISTRATION = {
    "UserModeDriverName": "viogpudxvkx.dll",
    "UserModeDriverNameWow": "viogpudxvk_x86.dll",
}
REGISTRATION = {
    "OpenGLDriverName": ("opengl", "viogpuopengl.dll", "arm64x", "0x00010000"),
    "OpenGLDriverNameWow": ("opengl", "viogpuopengl_x86.dll", "x86", "0x00010000"),
    "VulkanDriverName": ("opengl", "turnip.json", "data", "0x00000000"),
    "VulkanDriverNameWow": ("opengl", "turnip-wow.json", "data", "0x00000000"),
    "OpenCLDriverName": ("opencl", "viogpucl.dll", "arm64x", "0x00000000"),
    "OpenCLDriverNameWow": ("opencl", "viogpucl_x86.dll", "x86", "0x00000000"),
}
# D3D10/11 UMDs. The INX registers the native Mesa UMD (viogpud3d.dll), which
# emulated x64 processes cannot load; composition points UserModeDriverName at
# the ARM64X entry and adds the x86 UMD for WoW64. Kept apart from REGISTRATION:
# the installer's API contract covers only the ICD values.
D3D10_FILES = {
    "viogpud3dx.dll": ("arm64x", "icd"),
    "viogpud3d_x64.dll": ("x64", "runtime"),
    "viogpud3d_x86.dll": ("x86", "icd"),
}
D3D_REGISTRATION = {
    "UserModeDriverName": "viogpud3dx.dll",
    "UserModeDriverNameWow": "viogpud3d_x86.dll",
}


def d3d_registration_line(key, filename):
    # One entry each for the D3D9, D3D10 and D3D11 runtimes, as the INX has.
    return f"HKR,,{key},%REG_MULTI_SZ%," + ",".join([f'"%13%\\{filename}"'] * 3)


NATIVE_UMD_REGISTRATION = d3d_registration_line("UserModeDriverName", "viogpud3d.dll")


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


def build_candidate_umds(dxvk):
    require(os.name == "nt", "Candidate UMD auto-build is available only on Windows")
    require(dxvk is not None and dxvk.is_dir(), "The dxvk-umd job output (--dxvk) is required")
    runner_temp = os.environ.get("RUNNER_TEMP")
    require(runner_temp, "RUNNER_TEMP is required for candidate UMD auto-build")
    output = Path(runner_temp) / "droidvm-candidate-umd-package"
    script = Path(__file__).with_name("build_candidate_umds.ps1")
    require(script.is_file(), f"Missing candidate UMD builder: {script}")
    subprocess.run([
        "pwsh", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(script),
        "-OutputRoot", str(output),
        "-DxvkRoot", str(dxvk.resolve()),
        "-DxvkCommit", CANDIDATE_SOURCES["dxvk"],
        "-Vkd3dCommit", CANDIDATE_SOURCES["vkd3d"],
    ], check=True)
    return output


def read_candidate_umds(root):
    require(root.is_dir() and not root.is_symlink(), "Invalid candidate UMD directory")
    paths = list(root.iterdir())
    require(all(p.is_file() and not p.is_symlink() for p in paths),
            "Candidate UMD input must be flat regular files, no subdirectories/links")
    data = json.loads((root / CANDIDATE_MANIFEST).read_text(encoding="utf-8-sig"))
    require(data.get("schema") == 1 and data.get("activation") == "unregistered-candidate",
            "Wrong candidate UMD manifest schema/activation")
    require(data.get("sources") == CANDIDATE_SOURCES, "Candidate UMD source pin mismatch")
    files = data.get("files")
    require(isinstance(files, dict) and set(files) == set(CANDIDATE_UMDS),
            "Wrong candidate UMD inventory")
    require({p.name for p in paths} == set(CANDIDATE_UMDS) | {CANDIDATE_MANIFEST},
            "Candidate UMD manifest must cover the exact flat input")
    for name, (family, machine) in CANDIDATE_UMDS.items():
        entry = files[name]
        flat_name(name)
        require(entry.get("family") == family and entry.get("machine") == machine and
                entry.get("role") == "candidate-runtime", f"Wrong candidate metadata: {name}")
        require(isinstance(entry.get("sha256"), str) and
                re.fullmatch(r"[a-fA-F0-9]{64}", entry["sha256"]) and
                sha(root / name) == entry["sha256"].lower(), f"Changed candidate UMD: {name}")
        require(pe_machine(root / name) == MACHINES[machine], f"Candidate PE machine mismatch: {name}")
        if family == "dxvk":
            check_dxvk_candidate(name, entry)
    return data


def check_dxvk_candidate(name, entry):
    # The ARM64X entry has no gate of its own; it forwards to a runtime below.
    if name not in DXVK_VULKAN_LOADERS:
        return
    # An unregistered candidate is only coherent while its own admission gate
    # is closed; the DXVK job records the gate it observed.
    require(isinstance(entry.get("admission"), str) and entry["admission"].startswith(CLOSED_ADMISSION),
            f"DXVK candidate must record a closed admission gate: {name}")
    require(entry.get("vulkan_loader") == DXVK_VULKAN_LOADERS[name],
            f"DXVK candidate must resolve its private Vulkan loader: {name}")


def check_candidate_registration_unwritten(inf_text):
    # Every D3D registration line the INF carries (UserModeDriverName and Wow)
    # must name only non-candidate UMDs; the candidate mapping stays receipt data.
    for line in inf_text.splitlines():
        if re.match(r"(?i)\s*HKR\s*,\s*,\s*UserModeDriverName", line):
            require(not any(name.casefold() in line.casefold() for name in CANDIDATE_UMDS),
                    f"Candidate UMD written into the INF: {line.strip()}")


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
    for name, (machine, role) in D3D10_FILES.items():
        entry = manifests["d3d10"]["files"].get(name, {})
        require(entry.get("machine") == machine and entry.get("role") == role,
                f"Missing actual {machine} D3D10 user-mode driver: {name}")
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
    require(not re.search(r"(?i)OpenGLDriverName|OpenCLDriverName|VulkanDriverName|UserModeDriverNameWow", text),
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
    text = replace_directive(text, "VioGpuWddm_DeviceSettings", NATIVE_UMD_REGISTRATION,
        d3d_registration_line("UserModeDriverName", D3D_REGISTRATION["UserModeDriverName"]))
    text += "\n[VioGpuWddm_ApiFiles]\n" + "\n".join(files) + "\n"
    text += "\n[VioGpuWddm_ApiSettings]\n"
    for key, (_, filename, _, flags) in REGISTRATION.items():
        text += f'HKR,,{key},{flags},"%13%\\{filename}"\n'
    for suffix in ("", "Wow"):
        text += f"HKR,,OpenGLVersion{suffix},0x00010001,1\n"
        text += f"HKR,,OpenGLFlags{suffix},0x00010001,1\n"
    text += d3d_registration_line("UserModeDriverNameWow", D3D_REGISTRATION["UserModeDriverNameWow"]) + "\n"
    return text


def assemble(driver, gl, cl, d3d10, mesa, clvk, d3d10_mesa, candidates):
    require(driver.is_dir() and not driver.is_symlink(), "Missing staged driver directory")
    require(all(p.is_file() and not p.is_symlink() for p in driver.iterdir()),
            "Staged driver must contain flat regular files")
    require(not any(p.suffix.lower() == ".cat" for p in driver.iterdir()),
            "Cannot modify a driver directory after catalog creation")
    roots = {"opengl": gl, "opencl": cl, "d3d10": d3d10, "dxvk": candidates, "vkd3d": candidates}
    manifests = {"opengl": read_manifest(gl, "opengl", {"mesa": mesa}),
                 "opencl": read_manifest(cl, "opencl", {"clvk": clvk}),
                 "d3d10": read_manifest(d3d10, "d3d10", {"mesa": d3d10_mesa})}
    candidate_manifest = read_candidate_umds(candidates)
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
    for name, entry in candidate_manifest["files"].items():
        family = entry["family"]
        require(name.casefold() not in all_names, f"Candidate/driver collision: {name}")
        all_names.add(name.casefold())
        files[name] = {**entry, "family": family}
    registered = {value[1].casefold() for value in REGISTRATION.values()}
    registered |= {value.casefold() for value in D3D_REGISTRATION.values()}
    require(not registered.intersection(name.casefold() for name in CANDIDATE_UMDS),
            "Candidate UMD must not replace an active registered runtime")
    require(set(CANDIDATE_D3D_REGISTRATION.values()) <= set(CANDIDATE_UMDS),
            "Candidate D3D registration must name candidate UMDs")
    inf = driver / "viogpuwddm.inf"
    original = inf.read_text(encoding="utf-8-sig")
    updated = compose_inf(original, sorted(set(files) | {RECEIPT}, key=str.casefold))
    check_candidate_registration_unwritten(updated)
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
               "candidate_sources": candidate_manifest["sources"],
               "candidate_activation": candidate_manifest["activation"],
               "candidate_umds": {name: files[name] for name in CANDIDATE_UMDS},
               "inf_sha256": sha(inf), "api_files_before_signing": files,
               "public_loaders": public_loaders,
               "loader_probes": manifests["opencl"]["loader_probes"],
               "registration": {key: value[1] for key, value in REGISTRATION.items()},
               "d3d_registration": dict(D3D_REGISTRATION),
               "candidate_d3d_registration": dict(CANDIDATE_D3D_REGISTRATION)}
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


def check_candidates_in_receipt(inventory):
    require(inventory.get("candidate_sources") == CANDIDATE_SOURCES,
            "Wrong candidate UMD source pins")
    require(inventory.get("candidate_activation") == "unregistered-candidate",
            "Candidate UMDs must remain unregistered candidates")
    candidates = inventory.get("candidate_umds")
    require(isinstance(candidates, dict) and set(candidates) == set(CANDIDATE_UMDS),
            "Wrong candidate UMD receipt inventory")
    require(inventory.get("candidate_d3d_registration") == CANDIDATE_D3D_REGISTRATION,
            "Wrong opt-in candidate D3D registration mapping")
    registered = {name.casefold() for name in inventory.get("registration", {}).values()}
    registered |= {name.casefold() for name in inventory.get("d3d_registration", {}).values()}
    for name, (family, machine) in CANDIDATE_UMDS.items():
        entry = candidates[name]
        require(entry.get("family") == family and entry.get("machine") == machine and
                entry.get("role") == "candidate-runtime", f"Wrong candidate UMD receipt: {name}")
        require(name.casefold() not in registered, f"Candidate UMD became active registration: {name}")
        if family == "dxvk":
            check_dxvk_candidate(name, entry)


def finalize(driver):
    """Inventory signed PEs immediately before Inf2Cat; signature is checked by CI/install."""
    manifest_path = driver / RECEIPT
    require(driver.is_dir() and not driver.is_symlink(), "Invalid driver directory")
    inventory = json.loads(manifest_path.read_text(encoding="utf-8-sig"))
    require(inventory.get("schema") == 1 and inventory.get("layout") == "flat-driverstore" and
            inventory.get("phase") == "before-signing", "Expected fresh before-signing inventory")
    require(inventory.get("loader_probes") == LOADER_PROBES,
            "Missing/wrong public loader probe mapping")
    check_candidates_in_receipt(inventory)
    for arch, name in LOADER_PROBES.items():
        entry = inventory["api_files_before_signing"].get(name, {})
        require(entry.get("machine") == arch and entry.get("role") == "installer-helper",
                f"Missing/wrong installed {arch} public loader helper: {name}")
    for name, (_, machine) in CANDIDATE_UMDS.items():
        entry = inventory["api_files_before_signing"].get(name, {})
        require(entry.get("machine") == machine and entry.get("role") == "candidate-runtime",
                f"Missing/wrong candidate runtime: {name}")
        require(pe_machine(driver / name) == MACHINES[machine], f"Wrong candidate PE architecture: {name}")
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
    require(inventory.get("d3d_registration") == D3D_REGISTRATION, "Wrong D3D UMD registration mapping")
    for key, filename in D3D_REGISTRATION.items():
        require(text.splitlines().count(d3d_registration_line(key, filename)) == 1,
                f"Missing device-scoped D3D UMD registration: {key}")
    require(NATIVE_UMD_REGISTRATION not in text, "Native-only D3D UMD registration survived composition")
    check_candidate_registration_unwritten(text)
    for name, (machine, _) in D3D10_FILES.items():
        entry = inventory["api_files_before_signing"].get(name, {})
        require(entry.get("machine") == machine and pe_machine(driver / name) == MACHINES[machine],
                f"Wrong D3D10 user-mode driver architecture: {name}")
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
              "candidate_sources": inventory["candidate_sources"],
              "candidate_activation": inventory["candidate_activation"],
              "candidate_umds": inventory["candidate_umds"],
              "registration": inventory["registration"],
              "d3d_registration": inventory["d3d_registration"],
              "candidate_d3d_registration": inventory["candidate_d3d_registration"]}
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
    parser.add_argument("--d3d10", type=Path)
    parser.add_argument("--d3d10-mesa")
    parser.add_argument("--candidate-root", type=Path)
    parser.add_argument("--dxvk", type=Path, help="dxvk-umd CI job output, one directory per architecture")
    args = parser.parse_args()
    if args.finalize:
        receipt = finalize(args.driver)
        print(f"PASS final inventory of {len(receipt['files'])} driver files; catalog signing pending")
        return
    parser.error("--gl --cl --d3d10 --mesa --clvk --d3d10-mesa required for staging") if any(
        value is None for value in (args.gl, args.cl, args.d3d10, args.mesa, args.clvk, args.d3d10_mesa)) else None
    candidates = args.candidate_root if args.candidate_root is not None else build_candidate_umds(args.dxvk)
    receipt = assemble(args.driver, args.gl, args.cl, args.d3d10, args.mesa, args.clvk, args.d3d10_mesa, candidates)
    print(f"PASS staged {len(receipt['api_files_before_signing'])} flat API files in display INF")
    print("PASS D3D10/11 UMD registered through the ARM64X entry, with an x86 UMD for WoW64")
    print("PASS staged DXVK (ARM64X entry, arm64/x64/x86) and VKD3D (arm64) candidates flat and unregistered")
    print("PENDING PE/catalog signing, InfVerif, public-loader install, ABI and GPU tests")


if __name__ == "__main__":
    main()
