#!/usr/bin/env python3
"""Execute the production timestamp lifecycle method and reset negative control."""
from pathlib import Path
import argparse
import shutil
import subprocess
import tempfile
import time

parser = argparse.ArgumentParser()
parser.add_argument('--negative-control-reset', action='store_true')
args = parser.parse_args()
here = Path(__file__).resolve().parent
source = (here.parents[1] / 'viogpudo/viogpudo.cpp').read_text()
start = source.index('NTSTATUS VioGpuAdapter::QueryNativeGpuTimestamp(')
end = source.index('\nVIOGPU_HOST_CONTEXT_RESULT\nVioGpuAdapter::CreateNativeSubmitQueueLocked', start)
body = source[start:end]
if args.negative_control_reset:
    token = '!IsNativeContextGenerationCurrent(snapshot->Generation, snapshot->ResetGeneration)'
    assert body.count(token) == 2
    body = body.replace(token, 'false')
fixture = (here / 'timestamp_test.cpp').read_text().replace('// INSERT_PRODUCTION', body)
with tempfile.TemporaryDirectory(prefix='gpu-timestamp-') as temporary:
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
    # Windows antivirus can briefly retain the just-exited image.
    for attempt in range(21):
        try:
            binary.unlink()
            break
        except PermissionError:
            if attempt == 20:
                raise
            time.sleep(0.25)
    if args.negative_control_reset:
        if result.returncode != 1 or 'FAIL reset during timestamp read' not in result.stdout:
            raise SystemExit('Missing reset rejection was not detected')
        print('PASS negative control: missing epoch guards detected')
    else:
        raise SystemExit(result.returncode)
