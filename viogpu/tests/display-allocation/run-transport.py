#!/usr/bin/env python3
"""Execute the actual CtrlQueue exchange with bounded transport/allocator seams."""
from pathlib import Path
import shutil
import subprocess
import tempfile

here = Path(__file__).resolve().parent
common = here.parents[1] / "common"
source = (common / "viogpu_queue.cpp").read_text(encoding="utf-8")
signature = "VIOGPU_DVSA_EXCHANGE_RESULT CtrlQueue::ExchangeDisplayAllocationCommand("
assert source.count(signature) == 1
start = source.index(signature)
brace = source.index("{", start)
depth, end = 1, brace + 1
while depth:
    depth += (source[end] == "{") - (source[end] == "}")
    end += 1
production = source[start:end]
fixture = (here / "transport_test.cpp.in").read_text(encoding="utf-8")
assert fixture.count("// INSERT_PRODUCTION_EXCHANGE") == 1
fixture = fixture.replace("// INSERT_PRODUCTION_EXCHANGE", production)
with tempfile.TemporaryDirectory(prefix="viogpu-dvsa-transport-") as output:
    directory = Path(output)
    unit = directory / "transport.cpp"
    unit.write_text(fixture, encoding="utf-8")
    if shutil.which("cl"):
        exe = directory / "transport.exe"
        command = ["cl", "/nologo", "/EHsc", "/W4", "/WX", "/std:c++17", f"/I{common}", str(unit), f"/Fe{exe}"]
    else:
        exe = directory / "transport"
        command = ["clang++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                   "-fno-omit-frame-pointer", "-I", str(common), str(unit), "-o", str(exe)]
    subprocess.run(command, cwd=directory, check=True)
    subprocess.run([str(exe)], cwd=directory, check=True)
