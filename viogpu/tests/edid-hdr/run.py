#!/usr/bin/env python3
"""Build and run the CTA-861 HDR EDID builder test."""
import pathlib
import subprocess
import sys
import tempfile

here = pathlib.Path(__file__).resolve().parent
with tempfile.TemporaryDirectory() as out:
    binary = pathlib.Path(out) / 'edid_hdr_test'
    subprocess.run(['g++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
                    str(here / 'edid_hdr_test.cpp'), '-o', str(binary)], check=True)
    sys.exit(subprocess.run([str(binary)]).returncode)
