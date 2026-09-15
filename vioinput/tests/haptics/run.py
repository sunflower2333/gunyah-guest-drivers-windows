#!/usr/bin/env python3
"""Compile real core and ISR/DPC sources with host-side mocks, never a WDK substitute."""
from __future__ import annotations

import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile


def main() -> None:
    """Run bounded native tests and optionally prove the old locking bug is detected."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--negative-control", action="store_true")
    args = parser.parse_args()
    compiler = shutil.which(args.cc)
    if compiler is None:
        parser.error(f"C compiler not found: {args.cc}")
    here = Path(__file__).resolve().parent
    source = here.parents[1] / "sys"
    flags = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-g"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    with tempfile.TemporaryDirectory(prefix="vioinput-haptics-") as temp:
        work = Path(temp)
        # Only the Windows API surface is mocked; IsrDpc.c is copied unchanged.
        shutil.copy2(source / "IsrDpc.c", work / "IsrDpc.c")
        (work / "precomp.h").write_text("/* Host-test precompiled-header shim. */\n")
        (work / "vioinput.h").write_text("/* Mock declarations live in test_isrdpc.c. */\n")
        shutil.copy2(here / "test_isrdpc.c", work / "test_isrdpc.c")
        for name, path in (("core", here / "test_core.c"), ("isrdpc", work / "test_isrdpc.c")):
            exe = work / name
            subprocess.run([compiler, *flags, "-I", str(source), str(path), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True, timeout=30)
        if args.negative_control:
            # Reintroduce only completion-under-lock; the production-source harness must fail.
            path = work / "IsrDpc.c"
            text = path.read_text()
            old = "            WdfRequestComplete(request, STATUS_SUCCESS);"
            if text.count(old) != 1:
                raise RuntimeError("Negative-control anchor changed; update the mutation explicitly")
            text = text.replace(old, "            WdfSpinLockAcquire(pContext->StatusQLock);\n" + old +
                                "\n            WdfSpinLockRelease(pContext->StatusQLock);")
            path.write_text(text)
            exe = work / "negative"
            subprocess.run([compiler, *flags, "-I", str(source), str(work / "test_isrdpc.c"),
                            "-o", str(exe)], check=True)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
            if result.returncode == 0 or "!eventLock && !statusLock" not in result.stderr:
                raise RuntimeError(f"Expected the completion-lock assertion, got: {result.stderr}")
            print("negative control: completion-under-lock correctly rejected")


if __name__ == "__main__":
    main()
