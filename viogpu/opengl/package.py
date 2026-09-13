#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Verify pinned Mesa artifacts and assemble the system ICD payload/INF."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess

MESA_COMMIT = "91316f199e9c066109c7832e72653e65754349fa"
MESA_RUN = 34727218408
MACHINES = {"arm64": 0xAA64, "x64": 0x8664, "x86": 0x14C}
DLL_STEMS = ("viogpu_gl", "viogpu_egl", "viogpu_gles1", "viogpu_gles2",
             "viogpu_gl_vk", "viogpu_gl_loader")
GENERIC_PRIVATE = {"libgallium_wgl.dll", "libegl.dll", "libglesv1_cm.dll",
                   "libglesv2.dll", "vulkan_freedreno.dll", "vulkan-1.dll", "z-1.dll"}
EXPORTS = [line.split()[0] for line in Path(__file__).with_name("proxy-exports.txt").read_text().splitlines()]

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def dlls(arch):
    return tuple(f"{stem}_{arch}.dll" for stem in DLL_STEMS)

def pe_imports(path):
    """Read real import descriptors; filenames copied by staging are not proof."""
    data = path.read_bytes()
    pe = struct.unpack_from("<I", data, 0x3c)[0]
    sections = struct.unpack_from("<H", data, pe + 6)[0]
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    optional = pe + 24
    magic = struct.unpack_from("<H", data, optional)[0]
    directories = optional + (112 if magic == 0x20B else 96)
    table = optional + optional_size
    def offset(rva):
        for i in range(sections):
            size, address, raw_size, raw = struct.unpack_from("<IIII", data, table+i*40+8)
            if address <= rva < address + max(size, raw_size):
                return raw+rva-address
        raise ValueError(f"Import RVA outside sections: {path} {rva:x}")
    def name(rva):
        start = offset(rva)
        return data[start:data.index(0, start)].decode("ascii").lower()
    imports = set()
    rva, size = struct.unpack_from("<II", data, directories + 8)
    if rva:
        start = offset(rva)
        for at in range(start, start + size, 20):
            fields = struct.unpack_from("<IIIII", data, at)
            if not any(fields):
                break
            imports.add(name(fields[3]))
    # Delay imports are also architecture-sensitive, and cannot be hidden by
    # a successful initial LoadLibrary test.
    rva, size = struct.unpack_from("<II", data, directories + 13 * 8)
    if rva:
        start = offset(rva)
        for at in range(start, start + size, 32):
            fields = struct.unpack_from("<IIIIIIII", data, at)
            if not any(fields):
                break
            assert fields[0] & 1, (path, "unsupported VA delay import")
            imports.add(name(fields[1]))
    return imports

def verify_imports(path, arch):
    imports = pe_imports(path)
    assert not (imports & GENERIC_PRIVATE), (path, "generic dependency", imports & GENERIC_PRIVATE)
    private = {name for name in imports if name.startswith("viogpu_")}
    assert private <= set(dlls(arch)), (path, "wrong architecture dependency", private)
    if path.name.startswith(("viogpu_egl_", "viogpu_gles1_", "viogpu_gles2_")):
        assert f"viogpu_gl_{arch}.dll" in private, (path, "missing Gallium import")
    return sorted(imports)

def pe_exports(path, machine):
    data = path.read_bytes()
    assert data[:2] == b"MZ", path
    pe = struct.unpack_from("<I", data, 0x3c)[0]
    assert data[pe:pe+4] == b"PE\0\0", path
    actual, sections = struct.unpack_from("<HH", data, pe+4)
    assert actual == machine, (path, hex(actual), hex(machine))
    optional_size = struct.unpack_from("<H", data, pe+20)[0]
    optional = pe+24
    magic = struct.unpack_from("<H", data, optional)[0]
    directories = optional + (112 if magic == 0x20B else 96)
    export_rva, export_size = struct.unpack_from("<II", data, directories)
    section_table = optional+optional_size
    def offset(rva):
        for i in range(sections):
            size, address, raw_size, raw = struct.unpack_from("<IIII", data, section_table+i*40+8)
            if address <= rva < address + max(size, raw_size):
                return raw+rva-address
        raise ValueError(f"RVA outside sections: {path} {rva:x}")
    def string(rva):
        start = offset(rva)
        return data[start:data.index(0, start)].decode("ascii")
    if not export_rva:
        return {}
    directory = offset(export_rva)
    count = struct.unpack_from("<I", data, directory+24)[0]
    functions, names, ordinals = struct.unpack_from("<III", data, directory+28)
    result = {}
    for i in range(count):
        name = string(struct.unpack_from("<I", data, offset(names)+4*i)[0])
        ordinal = struct.unpack_from("<H", data, offset(ordinals)+2*i)[0]
        address = struct.unpack_from("<I", data, offset(functions)+4*ordinal)[0]
        assert address, (path, name)
        result[name] = string(address) if export_rva <= address < export_rva+export_size else None
    return result

def verify_payload(root, arch):
    assert (root/"source-commit.txt").read_text(encoding="utf-8-sig").strip() == MESA_COMMIT
    entries = (root/"SHA256SUMS.txt").read_text(encoding="utf-8-sig").splitlines()
    names = set()
    for line in entries:
        sha, name = line.split("  ", 1)
        assert Path(name).name == name and name.lower() not in names and digest(root/name) == sha, (root, name)
        names.add(name.lower())
    assert all(path.is_file() for path in root.iterdir()), root
    assert names == {path.name.lower() for path in root.iterdir()} - {"sha256sums.txt"}, (root, "unmanifested files")
    for dll in dlls(arch):
        exports = pe_exports(root/dll, MACHINES[arch])
        verify_imports(root/dll, arch)
        required = EXPORTS[:19] if dll == f"viogpu_gl_{arch}.dll" else EXPORTS[19:] if dll == f"viogpu_gl_vk_{arch}.dll" else []
        for name in required:
            assert name in exports and exports[name] is None, (arch, dll, name, "must be real code export")
    print(f"PASS {arch}: all {len(entries)} hashes, real private imports, PE machines, 19 GL ICD + 3 Vulkan ICD exports")

def assemble(args):
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    identity = {"mesa_commit": MESA_COMMIT, "mesa_run": MESA_RUN, "files": {}}
    runtime = {"schema": 1, "family": "opengl", "sources": {
        "mesa": MESA_COMMIT, "mesa_run": MESA_RUN,
        "parent": subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip(),
    }, "files": {}}
    def record(path, machine, role):
        runtime["files"][path.name] = {"sha256": digest(path), "machine": machine, "role": role}
    for arch, machine in MACHINES.items():
        source = args.payloads/f"opengl-zink-{arch}-candidate"
        verify_payload(source, arch)
        for dll in dlls(arch):
            shutil.copy2(source/dll, output/dll)
            identity["files"][dll] = digest(output/dll)
            record(output/dll, arch, "runtime")
        proxy = f"viogpuopengl_{arch}.dll"
        exports = pe_exports(args.proxies/proxy, machine)
        assert set(exports) == set(EXPORTS) and all(value is None for value in exports.values()), proxy
        shutil.copy2(args.proxies/proxy, output/proxy)
        record(output/proxy, arch, "icd" if arch == "x86" else "runtime")
        shutil.copy2(args.proxies/f"system-probe-{arch}.exe", output/f"system-probe-{arch}.exe")
        pe_exports(args.proxies/f"small-stack-probe-{arch}.exe", machine)
        shutil.copy2(args.proxies/f"small-stack-probe-{arch}.exe", output/f"small-stack-probe-{arch}.exe")
        pe_exports(args.proxies/f"gles-probe-{arch}.exe", machine)
        shutil.copy2(args.proxies/f"gles-probe-{arch}.exe", output/f"gles-probe-{arch}.exe")
        for stem in ("system-probe", "small-stack-probe", "gles-probe"):
            record(output/f"{stem}-{arch}.exe", arch, "probe")
    hybrid = args.proxies/"viogpuopengl.dll"
    exports = pe_exports(hybrid, MACHINES["arm64"])
    assert set(exports) == set(EXPORTS), "ARM64X export table mismatch"
    # Both views contain adapter code that loads exact sibling paths. A pure
    # forwarder would depend on the external application's DLL search path.
    assert all(value is None for value in exports.values()), exports
    shutil.copy2(hybrid, output/hybrid.name)
    record(output/hybrid.name, "arm64x", "icd")
    for suffix, dll in (("", "viogpuopengl.dll"), ("-wow", "viogpuopengl_x86.dll")):
        (output/f"turnip{suffix}.json").write_text(json.dumps({
            "file_format_version": "1.0.0", "ICD": {"api_version": "1.4.304", "library_path": ".\\"+dll}
        }, indent=2)+"\n")
        record(output/f"turnip{suffix}.json", "data", "icd")
    (output/"source-identity.json").write_text(json.dumps(identity, indent=2)+"\n")
    record(output/"source-identity.json", "data", "runtime")
    (output/"flat-runtime.json").write_text(json.dumps(runtime, indent=2)+"\n")
    print("PASS flat system ICD payload assembled; not device/runtime acceptance")

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--payloads", type=Path)
    parser.add_argument("--proxies", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--verify-only", action="store_true")
    args = parser.parse_args()
    if args.verify_only:
        for arch in MACHINES:
            verify_payload(args.payloads/f"opengl-zink-{arch}-candidate", arch)
    else:
        assemble(args)

if __name__ == "__main__":
    main()
