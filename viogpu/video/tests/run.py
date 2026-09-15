#!/usr/bin/env python3
"""Build and execute the real portable video core; no network or device needed."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def main() -> None:
    """Compile C separately from C++ so both WDK-facing C and fixture ABI are checked."""
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="viogpu-video-") as tmp:
        work = Path(tmp)
        if os.name == "nt":
            subprocess.run(["cl", "/nologo", "/TC", "/W4", "/WX", "/c",
                            str(root / "core/video_core.c"), "/Fo" + str(work / "core.obj")], check=True)
            subprocess.run(["cl", "/nologo", "/std:c++17", "/EHsc", "/W4", "/WX",
                            str(root / "tests/video_core_test.cpp"), str(work / "core.obj"),
                            "/Fe:" + str(work / "video_test.exe")], cwd=work, check=True)
            subprocess.run([str(work / "video_test.exe")], check=True)
        else:
            cc = os.environ.get("CC") or shutil.which("clang") or "cc"
            cxx = os.environ.get("CXX") or shutil.which("clang++") or "c++"
            flags = ["-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", "-g"]
            subprocess.run([cc, "-std=c11", *flags, "-c", str(root / "core/video_core.c"),
                            "-o", str(work / "core.o")], check=True)
            subprocess.run([cxx, "-std=c++17", *flags, str(root / "tests/video_core_test.cpp"),
                            str(work / "core.o"), "-o", str(work / "video_test")], check=True)
            subprocess.run([str(work / "video_test")], check=True)


if __name__ == "__main__":
    main()
