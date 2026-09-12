#!/usr/bin/env python3
"""Execute the actual CommitVidPn body against strict per-source VidPN peers."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

parser = argparse.ArgumentParser()
parser.add_argument('--negative-control-source-all', action='store_true')
args = parser.parse_args()
here = Path(__file__).resolve().parent
root = here.parents[2]
source = (root / 'viogpu/viogpudo/viogpudo.cpp').read_text()
start = source.index('NTSTATUS VioGpuDod::CommitVidPn(')
end = source.index('NTSTATUS VioGpuDod::SetSourceModeAndPath(', start)
production = source[start:end]
if args.negative_control_source_all:
    normalized = '? 0 : pCommitVidPn->AffectedVidPnSourceId;'
    assert production.count(normalized) == 1
    production = production.replace(normalized, '? D3DDDI_ID_ALL : pCommitVidPn->AffectedVidPnSourceId;')
fixture = (here / 'vidpn_commit_test.cpp').read_text().replace('// INSERT_PRODUCTION', production)
with tempfile.TemporaryDirectory(prefix='viogpu-vidpn-commit-') as temporary:
    output = Path(temporary)
    unit = output / 'test.cpp'
    unit.write_text(fixture)
    if shutil.which('cl'):
        binary = output / 'test.exe'
        command = ['cl', '/nologo', '/EHsc', '/W4', '/WX', '/std:c++17', str(unit), f'/Fe{binary}']
    else:
        binary = output / 'test'
        command = ['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                   '-fsanitize=address,undefined', '-fno-omit-frame-pointer', str(unit), '-o', str(binary)]
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
    if args.negative_control_source_all:
        if result.returncode != 1 or 'FAIL ID_ALL never enters per-source callbacks' not in result.stdout:
            raise SystemExit('Negative control did not detect lost all-source normalization')
        print('PASS negative control: lost all-source normalization detected')
    else:
        raise SystemExit(result.returncode)
