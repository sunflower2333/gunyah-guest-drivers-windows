#!/usr/bin/env python3
"""Execute production Present geometry and its actual rectangle-copy loop."""
from pathlib import Path
import argparse
import shutil
import subprocess
import tempfile
import time

parser = argparse.ArgumentParser()
parser.add_argument('--baseline', action='store_true')
parser.add_argument('--negative-control-raw-copy', action='store_true')
args = parser.parse_args()
if args.baseline and args.negative_control_raw_copy:
    parser.error('choose one negative control')
here = Path(__file__).resolve().parent
root = here.parents[2]
relative = 'viogpu/viogpuwddm/wddmddi.cpp'
source = (subprocess.check_output(['git', 'show', '875edab6e0beb2ad8a26f95cc162d97325ae5774:' + relative],
                                  cwd=root, text=True) if args.baseline else (root / relative).read_text())

def function(name):
    start = source.index(name + '(')
    start = source.rfind('\n', 0, start) + 1
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

parts = [function(name) for name in ('IsSupportedSurfaceFormat', 'ValidatePresentRect', 'ValidatePresentGeometry')]
if not args.baseline:
    parts.append(function('CopyPresentRow'))
execute = function('ExecutePresentTransaction')
start = execute.index('for (UINT index = 0; index < transaction->RectCount; ++index)')
end = execute.index('            KeMemoryBarrier();', start)
copy = execute[start:end]
if args.negative_control_raw_copy:
    old = 'CopyPresentRow(destinationBase + destinationOffset, sourceBase + sourceOffset,\n                                   rowBytes, source->Format, destination->Format);'
    assert copy.count(old) == 1
    copy = copy.replace(old, 'RtlCopyMemory(destinationBase + destinationOffset, sourceBase + sourceOffset, rowBytes);')
fixture = (here / 'present_formats_test.cpp').read_text()
fixture = fixture.replace('// INSERT_PRODUCTION', '\n\n'.join(parts)).replace('// INSERT_COPY', copy)
with tempfile.TemporaryDirectory(prefix='present-formats-') as temporary:
    output = Path(temporary)
    unit = output / 'test.cpp'
    unit.write_text(fixture)
    binary = output / ('test.exe' if shutil.which('cl') else 'test')
    command = (['cl', '/nologo', '/EHsc', '/W4', '/WX', '/std:c++17', str(unit), f'/Fe{binary}']
               if shutil.which('cl') else ['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                          '-fsanitize=undefined', str(unit), '-o', str(binary)])
    subprocess.run(command, cwd=output, check=True)
    result = subprocess.run([str(binary)], cwd=output, capture_output=True, text=True)
    print(result.stdout, end='')
    print(result.stderr, end='')
    for attempt in range(21):
        try:
            binary.unlink()
            break
        except PermissionError:
            if attempt == 20:
                raise
            time.sleep(0.25)
    if args.baseline or args.negative_control_raw_copy:
        if result.returncode != 1 or 'FAIL RGBA to BGRA' not in result.stdout or 'FAIL BGRX to BGRA' not in result.stdout:
            raise SystemExit('Missing color conversion was not detected')
        print('PASS negative control: actual production format/copy defect detected')
    else:
        raise SystemExit(result.returncode)
