#!/usr/bin/env python3
"""Compile and execute the production guest-scanout bounds contract."""
from pathlib import Path
import shutil
import subprocess
import tempfile

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
