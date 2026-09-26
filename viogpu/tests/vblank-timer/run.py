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
controls.add_argument('--negative-control-phase-grid', action='store_true')
controls.add_argument('--negative-control-enable', action='store_true')
controls.add_argument('--negative-control-scanline-epoch', action='store_true')
controls.add_argument('--negative-control-scanline-arm', action='store_true')
controls.add_argument('--negative-control-scanline-snapshot', action='store_true')
controls.add_argument('--negative-control-cadence-resync', action='store_true')
controls.add_argument('--negative-control-cadence-gates', action='store_true')
controls.add_argument('--negative-control-cadence-snapshot', action='store_true')
controls.add_argument('--negative-control-cadence-publish', action='store_true')
args = parser.parse_args()
here = Path(__file__).resolve().parent
root = here.parents[2]
source = (root / 'viogpu/viogpudo/viogpudo.cpp').read_text()
timing_header = (root / 'viogpu/common/display_timing.h').read_text()
callback = source[source.index('static VOID VioGpuCrtcVsyncDpcRoutine('):
                  source.index('PAGED_CODE_SEG_BEGIN')]
methods = source[source.index('NTSTATUS VioGpuDod::ArmCrtcVsyncTimer('):
                 source.index('NTSTATUS VioGpuDod::ControlInterrupt(')]
delivery = source[source.index('VOID VioGpuDod::DeliverCrtcVsync('):
                  source.index('BOOLEAN VioGpuDod::PrepareNativeSchedulerNotificationAtDirql(')]
scanline = source[source.index('NTSTATUS VioGpuDod::GetScanLine('):
                  source.index('NTSTATUS VioGpuDod::SetCrtcTiming(')]
timing = source[source.index('NTSTATUS VioGpuDod::SetCrtcTiming('):
                source.index('NTSTATUS VioGpuDod::ArmCrtcVsyncTimer(')]
expected_failure = None
if args.negative_control_phase_grid:
    original = '*nextDue = now + remaining;'
    assert timing_header.count(original) == 1
    timing_header = timing_header.replace(original,
        '*nextDue = now + (late >= static_cast<unsigned long long>(period) ? period : remaining);')
    expected_failure = 'FAIL stalled grid-preserving raster'
if args.negative_control_relative:
    original = 'VioGpuVsyncDelay100ns(now, m_CrtcNextDueTicks, frequency.QuadPart)'
    assert methods.count(original) == 1
    methods = methods.replace(original, 'VioGpuVsyncDelay100ns(now, now + m_CrtcPeriodTicks, frequency.QuadPart)')
    expected_failure = 'FAIL phase-aligned cadence'
if args.negative_control_enable:
    original = 'InterlockedCompareExchange(&m_CrtcVsyncEnabled, 0, 0) == 0'
    assert delivery.count(original) == 1
    delivery = delivery.replace(original, 'false')
    expected_failure = 'FAIL disabled vblank delivery'
if args.negative_control_scanline_epoch:
    original = 'const bool armed = InterlockedCompareExchange(&m_CrtcVsyncTimerArmed, 0, 0) != 0;'
    assert scanline.count(original) == 1
    scanline = scanline.replace(original, 'const bool armed = false;')
    expected_failure = 'FAIL armed raster grid'
if args.negative_control_scanline_arm:
    original = '    m_CrtcNextDueTicks = 0;'
    assert methods.count(original) == 1
    methods = methods.replace(original, '')
    expected_failure = 'FAIL unpublished arm raster'
if args.negative_control_scanline_snapshot:
    original = '    const LONGLONG nextDue = m_CrtcNextDueTicks;\n'
    assert scanline.count(original) == 1
    scanline = scanline.replace(original, '')
    original = '    KeReleaseSpinLock(&m_CrtcTimingLock, oldIrql);\n'
    assert scanline.count(original) == 1
    scanline = scanline.replace(original, original + '    const LONGLONG nextDue = m_CrtcNextDueTicks;\n')
    expected_failure = 'FAIL coherent raster snapshot'
if args.negative_control_cadence_resync:
    original = 'm_CrtcVblankCadence.ResyncPhaseTicks += skippedTicks;'
    assert methods.count(original) == 1
    methods = methods.replace(original, 'm_CrtcVblankCadence.ResyncPhaseTicks += late + (skippedTicks * 0);')
    expected_failure = 'FAIL cadence resync accounting'
if args.negative_control_cadence_gates:
    original = 'RecordCrtcVblankDelivery(VioGpuVblankDisabled);'
    assert delivery.count(original) == 1
    delivery = delivery.replace(original, 'RecordCrtcVblankDelivery(VioGpuVblankHardwareGated);')
    expected_failure = 'FAIL cadence gate accounting'
if args.negative_control_cadence_snapshot:
    original = '    snapshot.Counters = m_CrtcVblankCadence;\n    KeReleaseSpinLock(&m_CrtcTimingLock, oldIrql);'
    assert delivery.count(original) == 1
    delivery = delivery.replace(original, '    KeReleaseSpinLock(&m_CrtcTimingLock, oldIrql);\n    snapshot.Counters = m_CrtcVblankCadence;')
    expected_failure = 'FAIL cadence coherent snapshot'
if args.negative_control_cadence_publish:
    original = 'REG_BINARY, &snapshot, sizeof(snapshot))'
    assert delivery.count(original) == 1
    delivery = delivery.replace(original, 'REG_BINARY, &snapshot, sizeof(snapshot) - 8)')
    expected_failure = 'FAIL cadence atomic publication'
fixture = (here / 'vblank_timer_test.cpp').read_text()
fixture = fixture.replace('// INSERT_CALLBACK', callback).replace('// INSERT_METHODS', methods)
fixture = fixture.replace('// INSERT_DELIVERY', delivery)
fixture = fixture.replace('// INSERT_SCANLINE', scanline).replace('// INSERT_TIMING', timing)
with tempfile.TemporaryDirectory(prefix='viogpu-vblank-timer-') as temporary:
    output = Path(temporary)
    unit = output / 'test.cpp'
    unit.write_text(fixture)
    # Quoted include selects this exact production header, or its deliberate
    # negative mutation, without changing the source worktree.
    (output / 'display_timing.h').write_text(timing_header)
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
