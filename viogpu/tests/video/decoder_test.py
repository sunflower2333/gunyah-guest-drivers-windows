#!/usr/bin/env python3
"""Compile the real decoder state machine, then prove semantic regressions fail."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[3]
mutations = {
    "duplicate-input-return": (" || !inputBusy[event.Index]", ""),
    "truncated-frame": ("(event.BytesUsed && event.BytesUsed < layout.Bytes)", "false"),
    "codec-substitution": ("VmediaRead32(format.Data + 8) != VMEDIA_H264", "false"),
    "early-capture-return": ("if (event.BytesUsed) return Result::Frame;", "if (event.BytesUsed) { CaptureQueue(event.Index); return Result::Frame; }"),
    "timeout-is-eos": ("if (!received) return Result::NeedInput;", "if (!received) return Result::Drained;"),
    "close-error-hidden": ("return success ? Result::Ok : Result::Failed;", "return success ? Result::Ok : Result::Ok;"),
}
with tempfile.TemporaryDirectory(prefix="viogpu-decoder-") as tmp:
    tree = Path(tmp)
    (tree / "viogpu/tests/video").mkdir(parents=True)
    shutil.copytree(root / "viogpu/video", tree / "viogpu/video")
    shutil.copy(root / "viogpu/tests/video/decoder_test.cpp", tree / "viogpu/tests/video")
    header = tree / "viogpu/video/decoder_session.h"
    original = header.read_text()
    for name, mutation in [("production", None), *mutations.items()]:
        if mutation:
            before, after = mutation
            if original.count(before) != 1:
                raise SystemExit(f"{name}: mutation anchor must occur exactly once")
            header.write_text(original.replace(before, after))
        else:
            header.write_text(original)
        binary = tree / name
        subprocess.run([os.environ.get("CXX", "g++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", "-fno-pie", "-no-pie",
                        str(tree / "viogpu/tests/video/decoder_test.cpp"), "-o", str(binary)], check=True)
        result = subprocess.run([str(binary)], capture_output=True, text=True)
        if mutation:
            if result.returncode != 1 or "FAIL line" not in result.stderr:
                raise SystemExit(f"{name}: expected semantic test rejection, got {result.returncode}: {result.stderr}")
            print(f"PASS negative {name}: rejected by runtime assertions")
        elif result.returncode:
            raise SystemExit(result.stdout + result.stderr)
        else:
            print(result.stdout.strip())
