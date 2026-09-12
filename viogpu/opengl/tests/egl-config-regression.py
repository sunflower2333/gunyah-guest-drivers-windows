#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compile real WGL config construction and Mesa EGL selection without a GPU."""
import argparse
from pathlib import Path
import os
import re
import shutil
import subprocess
import sys
import tempfile


def definition(source, name):
    matches = list(re.finditer(r"(?m)^" + re.escape(name) + r"\(", source))
    if len(matches) != 1:
        raise ValueError(f"Expected one production definition: {name}")
    start = source.rfind("\n\n", 0, matches[0].start()) + 2
    opening = source.index("{", matches[0].end())
    depth = 0
    for pos in range(opening, len(source)):
        depth += (source[pos] == "{") - (source[pos] == "}")
        if not depth:
            return source[start:pos + 1]
    raise ValueError(f"Unterminated production definition: {name}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--negative", action="store_true", help="Use exact pre-fix WGL source")
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    mesa = here.parents[2] / "external/mesa"
    wgl_relative = "src/egl/drivers/wgl/egl_wgl.c"
    wgl = (subprocess.check_output(["git", "show", "668d598875209397ffcb69a109017609a76744c1:" + wgl_relative],
                                   cwd=mesa, text=True) if args.negative else (mesa / wgl_relative).read_text())
    production = "\n\n".join(definition(wgl, name) for name in ("wgl_match_config", "wgl_add_config"))
    formats = mesa / "src/util/format"
    generator = [sys.executable, str(formats / "u_format_table.py")]
    generated = subprocess.check_output(generator + [str(formats / "u_format.yaml")], text=True)
    table_start = generated.index("static const struct util_format_description\nutil_format_descriptions[")
    table_end = generated.index("\n};", table_start) + 3
    production_formats = generated[table_start:table_end] + "\n" + definition(generated, "util_format_description")
    production_formats += "\n" + definition((formats / "u_format.c").read_text(), "util_format_is_float")
    fixture = (here / "egl-config-regression.c").read_text()
    fixture = fixture.replace("// PRODUCTION_FORMATS", production_formats).replace("// PRODUCTION_WGL", production)
    with tempfile.TemporaryDirectory(prefix="egl-config-regression-") as temp:
        output = Path(temp)
        generated_include = output / "util/format"
        generated_include.mkdir(parents=True)
        (generated_include / "u_format_gen.h").write_text(subprocess.check_output(
            generator + ["--enums", str(formats / "u_format.yaml")], text=True))
        unit = output / "fixture.c"
        unit.write_text(fixture)
        includes = [output, mesa / "include", mesa / "src", mesa / "src/egl/main",
                    mesa / "src/gallium/include", mesa / "src/gallium/frontends/wgl"]
        if os.name == "nt":
            if args.sanitize:
                parser.error("Sanitizers require the Linux compiler")
            exe = output / "fixture.exe"
            command = ["clang-cl", "/nologo", "/TC", "/clang:-std=c11", "/W3", "/WX",
                       "/DHAVE_STRUCT_TIMESPEC", "/DHAVE_TIMESPEC_GET", "/DUSE_GCC_ATOMIC_BUILTINS",
                       "/clang:-Wno-sign-compare",
                       "/DWIN32_LEAN_AND_MEAN", "/DNOMINMAX", str(unit), f"/Fe{exe}"]
            command += [f"/I{path}" for path in includes]
        else:
            exe = output / "fixture"
            compiler = shutil.which("clang") or "cc"
            command = [compiler, "-std=c11", "-D_GNU_SOURCE", "-DEGL_NO_X11", "-DUSE_GCC_ATOMIC_BUILTINS",
                       "-DUTIL_ARCH_LITTLE_ENDIAN=1", "-DUTIL_ARCH_BIG_ENDIAN=0",
                       "-DHAVE_PTHREAD", "-DHAVE_STRUCT_TIMESPEC", "-DHAVE_TIMESPEC_GET",
                       "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-Wno-sign-compare", str(unit), "-o", str(exe)]
            command += ["-I" + str(path) for path in includes]
            if args.sanitize:
                # Existing EGL array callbacks intentionally erase pointer types.
                # Keep all remaining sanitizer findings fatal, including leaks.
                command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
                if "clang" in Path(compiler).name:
                    command += ["-fno-sanitize=function"]
        subprocess.run(command, cwd=output, check=True)
        return subprocess.run([str(exe)], cwd=output).returncode


if __name__ == "__main__":
    raise SystemExit(main())
