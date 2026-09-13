#!/usr/bin/env python3
"""Execute production WDDM2 topology/segment/allocation policy with bounded buffers."""
from pathlib import Path
import argparse
import os
import shutil
import subprocess
import tempfile
import time

parser = argparse.ArgumentParser()
parser.add_argument('--wdk', action='store_true', help='Execute against actual WDK declarations on MSVC')
args = parser.parse_args()

here = Path(__file__).resolve().parent
root = here.parents[2]
text = (root / 'viogpu/viogpuwddm/wddmddi.cpp').read_text()

def function(signature):
    start = text.index(signature)
    brace = text.index('{', start)
    end, depth = brace + 1, 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]

production = '\n'.join(function(signature) for signature in (
    'VOID InitializeAllocationInfo(',
    'static NTSTATUS QuerySegment4(',
    'static NTSTATUS QueryPhysicalAdapterCaps(',
    'static NTSTATUS QueryHistoryBufferPrecision(',
    'static NTSTATUS QueryDisplayDriverCapsExtension(',
    '_Use_decl_annotations_ NTSTATUS APIENTRY VioGpuWddmGetNodeMetadata(',
))
unit = (here / ('wdk_contract_test.cpp' if args.wdk else 'wddm2_accounting_test.cpp')).read_text().replace('// INSERT_PRODUCTION', production)
with tempfile.TemporaryDirectory(prefix='viogpu-wddm2-accounting-') as output:
    directory = Path(output)
    source = directory / 'test.cpp'
    source.write_text(unit)
    if shutil.which('cl'):
        exe = directory / 'test.exe'
        command = ['cl', '/nologo', '/EHsc', '/W4', '/WX', '/std:c++17', str(source), f'/Fe{exe}']
        if args.wdk:
            kit = Path(os.environ['DROIDVM_KIT_ROOT']) / 'Include' / os.environ['DROIDVM_KIT_VERSION']
            command += ['/D_AMD64_', '/DAMD64', '/DWINVER=0x0A00', '/D_WIN32_WINNT=0x0A00',
                        '/DDXGKDDI_INTERFACE_VERSION=DXGKDDI_INTERFACE_VERSION_WDDM2_0',
                        *[f'/I{kit / part}' for part in ('km', 'shared', 'um')],
                        f'/I{root / "viogpu" / "common"}']
    else:
        if args.wdk:
            parser.error('--wdk requires actual Windows MSVC and WDK')
        exe = directory / 'test'
        command = ['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', str(source), '-o', str(exe)]
    subprocess.run(command, cwd=directory, check=True)
    subprocess.run([str(exe)], cwd=directory, check=True)
    # The ARM64 CI host may briefly retain the emulated x64 image after exit.
    for attempt in range(21):
        try:
            exe.unlink()
            break
        except PermissionError:
            if attempt == 20:
                raise
            time.sleep(0.25)
