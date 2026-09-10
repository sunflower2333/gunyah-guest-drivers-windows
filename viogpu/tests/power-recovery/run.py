#!/usr/bin/env python3
"""Execute the production D0 recovery function with controlled OS/transport peers."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

parser = argparse.ArgumentParser()
parser.add_argument('--revision', help='Read an older source revision for a negative control')
args = parser.parse_args()
here = Path(__file__).resolve().parent
root = here.parents[2]
source_path = 'viogpu/viogpudo/viogpudo.cpp'
source = (subprocess.check_output(['git', 'show', f'{args.revision}:{source_path}'], cwd=root, text=True)
          if args.revision else (root / source_path).read_text())
power = source[source.index('NTSTATUS VioGpuDod::SetPowerState('):
               source.index('NTSTATUS VioGpuDod::ResetFromTimeout(')]
header = (root / 'viogpu/viogpudo/viogpudo.h').read_text()
start = header.index('BOOLEAN IsHardwareInterruptDispatchAllowed(void) const')
predicate = header[start:header.index('\n    }', start) + len('\n    }')]
fixture = (here / 'power_recovery_test.cpp').read_text()
fixture = fixture.replace('// INSERT_INTERRUPT_PREDICATE', predicate).replace('// INSERT_POWER_FUNCTION', power)
with tempfile.TemporaryDirectory(prefix='viogpu-power-recovery-') as output:
    directory = Path(output)
    unit = directory / 'power_recovery_test.cpp'
    unit.write_text(fixture)
    if shutil.which('cl'):
        exe = directory / 'power_recovery_test.exe'
        command = ['cl', '/nologo', '/EHsc', '/W4', '/WX', '/std:c++17', str(unit), f'/Fe{exe}']
    else:
        exe = directory / 'power_recovery_test'
        command = ['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', str(unit), '-o', str(exe)]
    subprocess.run(command, cwd=directory, check=True)
    result = subprocess.run([str(exe)], cwd=directory).returncode
    # Windows ARM64 may briefly retain the exited x64 image. Retry only the
    # disposable executable; never replace the test result with cleanup noise.
    for attempt in range(21):
        try:
            exe.unlink()
            break
        except PermissionError:
            if attempt == 20:
                raise
            time.sleep(0.25)
    raise SystemExit(result)
