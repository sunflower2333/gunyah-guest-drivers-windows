#!/usr/bin/env python3
"""Execute actual monitor enumeration and signal conversion with OS peers."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--negative-control-duplicate', action='store_true')
parser.add_argument('--negative-control-add-error', action='store_true')
args = parser.parse_args()
here = Path(__file__).resolve().parent
root = here.parents[2]
source = (root / 'viogpu/viogpudo/viogpudo.cpp').read_text()

def span(first, after):
    begin = source.index(first)
    return source[begin:source.index(after, begin)]

production = span('VOID VioGpuDod::BuildVideoSignalInfo(', 'NTSTATUS VioGpuDod::AddSingleTargetMode(')
production += span('NTSTATUS VioGpuDod::AddSingleMonitorMode(', 'NTSTATUS VioGpuDod::EnumVidPnCofuncModality(')
if args.negative_control_duplicate:
    marker = 'Status = STATUS_SUCCESS; // Duplicate released; enumerate the remaining modes.'
    assert production.count(marker) == 1
    production = production.replace(marker, 'return STATUS_SUCCESS;')
if args.negative_control_add_error:
    marker = '''if (Status != STATUS_GRAPHICS_MODE_ALREADY_IN_MODESET)
            {
                return Status;
            }'''
    assert production.count(marker) == 1
    production = production.replace(marker, 'Status = TempStatus; // Reproduce masked add error.')
fixture = (here / 'monitor_modes_test.cpp').read_text()
assert fixture.count('// INSERT_PRODUCTION') == 1
fixture = fixture.replace('// INSERT_PRODUCTION', production)
with tempfile.TemporaryDirectory(prefix='viogpu-monitor-modes-') as temporary:
    output = Path(temporary)
    unit = output / 'test.cpp'
    unit.write_text(fixture)
    include = str(root / 'viogpu/common')
    if shutil.which('cl'):
        binary = output / 'test.exe'
        command = ['cl', '/nologo', '/EHsc', '/W4', '/WX', '/std:c++17', f'/I{include}', str(unit), f'/Fe{binary}']
    else:
        binary = output / 'test'
        command = ['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                   '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                   f'-I{include}', str(unit), '-o', str(binary)]
    subprocess.run(command, cwd=output, check=True)
    result = subprocess.run([str(binary)], cwd=output, capture_output=True, text=True)
    print(result.stdout, end='')
    print(result.stderr, end='')
    expected = None
    if args.negative_control_duplicate:
        expected = 'FAIL existing preferred mode still admits missing165Hz'
    if args.negative_control_add_error:
        expected = 'FAIL later add error survives successful cleanup'
    if expected:
        if result.returncode != 1 or expected not in result.stdout:
            raise SystemExit('Negative control did not detect the intended defect')
        print('PASS semantic negative control: ' + expected[5:])
    else:
        raise SystemExit(result.returncode)
