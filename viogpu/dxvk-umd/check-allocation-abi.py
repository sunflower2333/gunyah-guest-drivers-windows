#!/usr/bin/env python3
"""Compare the pinned DXVK production payload with the actual KMD ABI."""

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument("--dxvk-root", type=Path, default=Path("external/dxvk"))
args = parser.parse_args()
driver = Path(__file__).resolve().parents[1]
header = args.dxvk_root.resolve() / "src/umd/umd_allocation.h"
source = header.read_text()
# Extract the actual production wire declaration without the Windows-only
# callback classes. Do not replicate a potentially different test layout.
matches = re.findall(r"#pragma pack\(push, 4\)\s*struct AllocationInfo\s*\{.*?#pragma pack\(pop\)",
                     source, re.S)
if len(matches) != 1:
    raise SystemExit("Expected one production DXVK allocation wire declaration")
pairs = {
    "magic": "Header.Magic", "version": "Header.Version", "headerSize": "Header.Size",
    "reserved": "Header.Reserved", "size": "Size", "alignment": "Alignment",
    "requestedIova": "RequestedIova", "resetGeneration": "ExpectedResetGeneration",
    "flags": "Flags", "format": "Format", "width": "Width", "height": "Height",
    "pitch": "Pitch", "refreshNumerator": "RefreshRateNumerator",
    "refreshDenominator": "RefreshRateDenominator", "contextId": "ContextId",
}
checks = ["static_assert(sizeof(AllocationInfo) == sizeof(VIOGPU_WDDM_ALLOCATION_INFO));"]
for child, parent in pairs.items():
    checks.append(f"static_assert(offsetof(AllocationInfo, {child}) == "
                  f"offsetof(VIOGPU_WDDM_ALLOCATION_INFO, {parent}));")
    checks.append(f"static_assert(sizeof(AllocationInfo::{child}) == "
                  f"sizeof(((VIOGPU_WDDM_ALLOCATION_INFO*)nullptr)->{parent}));")
checks += [
    "constexpr AllocationInfo initial;",
    "static_assert(initial.magic == VIOGPU_WDDM_ABI_MAGIC);",
    "static_assert(initial.version == VIOGPU_WDDM_ABI_VERSION);",
    "static_assert(initial.headerSize == sizeof(VIOGPU_WDDM_ALLOCATION_INFO));",
    "static_assert(initial.flags == VIOGPU_WDDM_ALLOCATION_CPU_VISIBLE);",
    "static_assert(initial.alignment == 4096);",
    "static_assert(VIOGPU_WDDM_FORMAT_B8G8R8A8_UNORM == 1);",
    "static_assert(VIOGPU_WDDM_FORMAT_R8G8B8A8_UNORM == 3);",
]
translation = "\n".join([
    "#include <cstddef>", "#include <cstdint>",
    '#include "viogpu_wddm_abi.h"', matches[0], *checks, "int main() { return 0; }",
])
compiler = os.environ.get("CXX") or ("cl" if os.name == "nt" else "c++")
if not shutil.which(compiler):
    raise SystemExit(f"C++ compiler unavailable: {compiler}")
with tempfile.TemporaryDirectory(prefix="dxvk-allocation-abi-") as temporary:
    directory = Path(temporary)
    unit = directory / "allocation.cpp"
    unit.write_text(translation)
    if Path(compiler).stem.lower() == "cl":
        command = [compiler, "/nologo", "/std:c++17", "/EHsc", "/WX",
                   f"/I{driver / 'shared'}", str(unit), f"/Fe:{directory / 'abi.exe'}"]
    else:
        command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                   "-I", str(driver / "shared"), str(unit), "-o", str(directory / "abi")]
    subprocess.run(command, cwd=directory, check=True)
print(f"DXVK/KMD allocation ABI PASS: size, {len(pairs)} field offsets/widths, v0 defaults and formats")
