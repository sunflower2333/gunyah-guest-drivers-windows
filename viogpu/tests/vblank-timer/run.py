#!/usr/bin/env python3
"""Execute the production timer lifecycle against the documented EX_TIMER contract."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

parser = argparse.ArgumentParser()
controls = parser.add_mutually_exclusive_group()
controls.add_argument('--negative-control-relative', action='store_true')
controls.add_argument('--negative-control-enable', action='store_true')
args = parser.parse_args()
here = Path(__file__).resolve().parent
root = here.parents[2]
source = (root / 'viogpu/viogpudo/viogpudo.cpp').read_text()
callback = source[source.index('static VOID VioGpuCrtcVsyncDpcRoutine('):
                  source.index('PAGED_CODE_SEG_BEGIN')]
methods = source[source.index('NTSTATUS VioGpuDod::ArmCrtcVsyncTimer('):
                 source.index('NTSTATUS VioGpuDod::ControlInterrupt(')]
delivery = source[source.index('VOID VioGpuDod::DeliverCrtcVsync('):
                  source.index('BOOLEAN VioGpuDod::PrepareNativeSchedulerNotificationAtDirql(')]
expected_failure = None
if args.negative_control_relative:
    original = 'VioGpuVsyncDelay100ns(now, m_CrtcNextDueTicks, frequency.QuadPart)'
    assert methods.count(original) == 1
    methods = methods.replace(original, 'VioGpuVsyncDelay100ns(now, now + m_CrtcPeriodTicks, frequency.QuadPart)')
    expected_failure = 'FAIL phase-aligned cadence'
if args.negative_control_enable:
    original = 'InterlockedCompareExchange(&m_CrtcVsyncEnabled, 0, 0) == 0 || '
    assert delivery.count(original) == 1
    delivery = delivery.replace(original, '')
    expected_failure = 'FAIL disabled vblank delivery'
fixture = (here / 'vblank_timer_test.cpp').read_text()
fixture = fixture.replace('// INSERT_CALLBACK', callback).replace('// INSERT_METHODS', methods)
fixture = fixture.replace('// INSERT_DELIVERY', delivery)
with tempfile.TemporaryDirectory(prefix='viogpu-vblank-timer-') as temporary:
    output = Path(temporary)
    unit = output / 'test.cpp'
    unit.write_text(fixture)
    if shutil.which('cl'):
        binary = output / 'test.exe'
        command = ['cl', '/nologo', '/EHsc', '/W4', '/WX', '/std:c++17',
                   '/I' + str(root / 'viogpu/common'), str(unit), '/Fe' + str(binary)]
    else:
        binary = output / 'test'
        command = ['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pthread',
                   '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                   '-I' + str(root / 'viogpu/common'), str(unit), '-o', str(binary)]
    subprocess.run(command, cwd=output, check=True)
    result = subprocess.run([str(binary)], cwd=output, timeout=20, capture_output=True, text=True)
    print(result.stdout, end='')
    print(result.stderr, end='')
    # Match existing host fixtures: Windows can briefly retain an exited image.
    for attempt in range(21):
        try:
            binary.unlink()
            break
        except PermissionError:
            if attempt == 20:
                raise
            time.sleep(0.25)
    if expected_failure:
        if result.returncode == 0 or expected_failure not in result.stdout:
            raise SystemExit('Negative control missed its intended semantic failure: ' + expected_failure)
        print('PASS negative control: ' + expected_failure)
    else:
        raise SystemExit(result.returncode)
