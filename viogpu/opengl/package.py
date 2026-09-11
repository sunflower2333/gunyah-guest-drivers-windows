#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Verify pinned Mesa artifacts and assemble the system ICD payload/INF."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct

MESA_COMMIT = "e0caf8c6740638c1b12a5150edca095d01262805"
MESA_RUN = 34602261501
MACHINES = {"arm64": 0xAA64, "x64": 0x8664, "x86": 0x14C}
DLLS = ("libgallium_wgl.dll", "libEGL.dll", "libGLESv1_CM.dll", "libGLESv2.dll",
        "vulkan_freedreno.dll", "vulkan-1.dll", "z-1.dll")
EXPORTS = [line.split()[0] for line in Path(__file__).with_name("proxy-exports.txt").read_text().splitlines()]

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

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
    assert len(entries) == 39, (root, len(entries))
    for line in entries:
        sha, name = line.split("  ", 1)
        assert Path(name).name == name and digest(root/name) == sha, (root, name)
    for dll in DLLS:
        exports = pe_exports(root/dll, MACHINES[arch])
        required = EXPORTS[:19] if dll == "libgallium_wgl.dll" else EXPORTS[19:] if dll == "vulkan_freedreno.dll" else []
        for name in required:
            assert name in exports and exports[name] is None, (arch, dll, name, "must be real code export")
    print(f"PASS {arch}: all 39 hashes, PE machines, 19 real GL ICD + 3 real Vulkan ICD exports")

def assemble(args):
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    identity = {"mesa_commit": MESA_COMMIT, "mesa_run": MESA_RUN, "files": {}}
    for arch, machine in MACHINES.items():
        source = args.payloads/f"opengl-zink-{arch}-candidate"
        verify_payload(source, arch)
        dest = output/arch
        dest.mkdir()
        for dll in DLLS:
            shutil.copy2(source/dll, dest/dll)
            identity["files"][f"{arch}/{dll}"] = digest(dest/dll)
        proxy = f"viogpuopengl_{arch}.dll"
        exports = pe_exports(args.proxies/proxy, machine)
        assert set(exports) == set(EXPORTS) and all(value is None for value in exports.values()), proxy
        shutil.copy2(args.proxies/proxy, output/proxy)
        shutil.copy2(args.proxies/f"system-probe-{arch}.exe", output/f"system-probe-{arch}.exe")
    hybrid = args.proxies/"viogpuopengl.dll"
    exports = pe_exports(hybrid, MACHINES["arm64"])
    assert set(exports) == set(EXPORTS), "ARM64X export table mismatch"
    # Both views contain adapter code that loads exact sibling paths. A pure
    # forwarder would depend on the external application's DLL search path.
    assert all(value is None for value in exports.values()), exports
    shutil.copy2(hybrid, output/hybrid.name)
    for suffix, dll in (("", "viogpuopengl.dll"), ("-wow", "viogpuopengl_x86.dll")):
        (output/f"turnip{suffix}.json").write_text(json.dumps({
            "file_format_version": "1.0.0", "ICD": {"api_version": "1.4.304", "library_path": ".\\"+dll}
        }, indent=2)+"\n")
    (output/"source-identity.json").write_text(json.dumps(identity, indent=2)+"\n")
    print("PASS system ICD payload assembled; not device/runtime acceptance")

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
