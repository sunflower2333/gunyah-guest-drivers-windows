#!/usr/bin/env python3
"""Compile and execute the production guest-scanout bounds contract."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

source = Path(__file__).resolve().with_name("guest_scanout_test.cpp")
with tempfile.TemporaryDirectory(prefix="viogpu-guest-scanout-") as temporary:
    directory = Path(temporary)
    if shutil.which("cl"):
        binary = directory / "test.exe"
        command = ["cl", "/nologo", "/EHsc", "/W4", "/WX", "/std:c++17", str(source), f"/Fe{binary}"]
    else:
        binary = directory / "test"
        command = ["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                   "-fsanitize=address,undefined", "-fno-omit-frame-pointer", str(source), "-o", str(binary)]
    subprocess.run(command, cwd=directory, check=True)
    subprocess.run([str(binary)], cwd=directory, check=True)
    # Windows runners can retain the exited image briefly (AV/image cleanup).
    # Keep cleanup strict, with the same bounded retry as the other fixtures.
    for attempt in range(21):
        try:
            binary.unlink()
            break
        except PermissionError:
            if attempt == 20:
                raise
            time.sleep(0.25)
