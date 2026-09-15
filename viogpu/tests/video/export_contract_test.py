#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Exercise the real WDDM checker and prove it rejects missing/unknown exports."""
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
DEFINITION = ROOT / "viogpu/viogpud3d/viogpud3d.def"
CHECKER = ROOT / "viogpu/viogpuwddm/check-contract.py"


def check_definition(expect_success: bool) -> None:
    """Run the production checker, preserving all its non-video constraints."""
    result = subprocess.run(
        [sys.executable, str(CHECKER)], cwd=ROOT, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=240,
        check=False,
    )
    if expect_success:
        if result.returncode:
            raise RuntimeError(result.stdout)
    elif result.returncode == 0 or "module definition must export exactly" not in result.stdout:
        raise RuntimeError("export negative control was not rejected by the intended check:\n" + result.stdout)


def main() -> None:
    """Restore the production definition after every negative control, even on failure."""
    original = DEFINITION.read_bytes()
    try:
        check_definition(True)
        text = original.decode("utf-8")
        for name in ("OpenAdapter", "VioGpuVideoQueue"):
            lines = text.splitlines(keepends=True)
            mutated = "".join(line for line in lines if line.strip() != name)
            if mutated == text:
                raise RuntimeError("missing negative-control target " + name)
            DEFINITION.write_text(mutated, encoding="utf-8")
            check_definition(False)
        DEFINITION.write_text(text + "    OpenAdapter12\n", encoding="utf-8")
        check_definition(False)
    finally:
        DEFINITION.write_bytes(original)
    print("PASS: real WDDM contract, required legacy/video exports and unknown-DDI rejection")


if __name__ == "__main__":
    main()
