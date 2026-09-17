#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Assemble the flat D3D10/11 multi-architecture UMD payload and its manifest.

Input: the ARM64X entry and probes from build-front.ps1, and the x64/x86 Mesa
UMD artifacts. The native ARM64 Mesa UMD (viogpud3d.dll) is not part of this
payload: it keeps its own staging and symbol path in the driver package.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct

MACHINES = {"arm64": 0xAA64, "arm64x": 0xAA64, "x64": 0x8664, "x86": 0x14C}
PAYLOAD = {
    "viogpud3dx.dll": ("arm64x", "icd"),
    "viogpud3d_x64.dll": ("x64", "runtime"),
    "viogpud3d_x86.dll": ("x86", "icd"),
    "d3d-umd-probe-arm64.exe": ("arm64", "probe"),
    "d3d-umd-probe-x64.exe": ("x64", "probe"),
    "d3d-umd-probe-x86.exe": ("x86", "probe"),
}
SYMBOLS = ("viogpud3dx.pdb", "viogpud3d_x64.pdb", "viogpud3d_x86.pdb")


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def machine(path):
    data = path.read_bytes()
    offset = struct.unpack_from("<I", data, 0x3C)[0]
    assert data[:2] == b"MZ" and data[offset:offset+4] == b"PE\0\0", path
    return struct.unpack_from("<H", data, offset + 4)[0]


def find(roots, name):
    matches = [p for root in roots for p in root.rglob(name) if p.is_file()]
    assert len(matches) == 1, (name, matches)
    return matches[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--inputs", type=Path, nargs="+", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--symbols", type=Path, required=True)
    parser.add_argument("--mesa", required=True)
    parser.add_argument("--parent", required=True)
    args = parser.parse_args()
    for value in (args.mesa, args.parent):
        assert re.fullmatch(r"[a-f0-9]{40}", value), value
    args.output.mkdir(parents=True, exist_ok=False)
    args.symbols.mkdir(parents=True, exist_ok=False)
    files = {}
    for name, (arch, role) in PAYLOAD.items():
        source = find(args.inputs, name)
        assert machine(source) == MACHINES[arch], (name, hex(machine(source)))
        shutil.copy2(source, args.output / name)
        files[name] = {"sha256": sha(args.output / name), "machine": arch, "role": role}
    for arch in ("x64", "x86"):
        data = (args.output / f"viogpud3d_{arch}.dll").read_bytes()
        # Zink must open the architecture's private loader next to the UMD; the
        # public vulkan-1.dll is not guaranteed to have an x64 or x86 view.
        assert f"viogpu_gl_loader_{arch}.dll".encode() in data, f"{arch} UMD is not linked to its private loader"
    for name in SYMBOLS:
        shutil.copy2(find(args.inputs, name), args.symbols / name)
    manifest = {"schema": 1, "family": "d3d10",
                "sources": {"mesa": args.mesa, "parent": args.parent}, "files": files}
    (args.output / "flat-runtime.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"PASS staged {len(files)} D3D10 multi-architecture files; ABI loading is checked on ARM64")


if __name__ == "__main__":
    main()
