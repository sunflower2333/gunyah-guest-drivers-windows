#!/usr/bin/env python3
"""Exercise the actual timer callback/arm/disarm bodies and deadline helper."""
from pathlib import Path
import resource
import subprocess
import tempfile

resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
here = Path(__file__).resolve().parent
gpu = here.parents[1]
source = (gpu / 'viogpudo/viogpudo.cpp').read_text()


def function(name):
    marker = name + '('
    start = source.index(marker)
    while source[source.rfind('\n', 0, start) + 1:start].strip() not in ('VOID', 'NTSTATUS', 'static VOID'):
        start = source.index(marker, start + 1)
    start = source.rfind('\n', 0, start) + 1
    brace = source.index('{', start)
    end, depth = brace + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


names = ['VioGpuCrtcVsyncDpcRoutine', 'VioGpuDod::OnCrtcVsyncTimer',
         'VioGpuDod::ArmCrtcVsyncTimer', 'VioGpuDod::DisarmCrtcVsyncTimer',
         'VioGpuDod::GetScanLine']
fixture = (here / 'test.cpp').read_text().replace('// PRODUCTION', '\n\n'.join(map(function, names)))
clock = (gpu / 'common/vblank_clock.h').read_text().replace('#pragma once', '')
fixture = fixture.replace('// CLOCK', clock)
controls = {
    'production': fixture,
    'latency-drift': fixture.replace('period - (now - clock.Deadline100ns) % period', 'period'),
    'periodic-rearm': fixture.replace('ExSetTimer(timer, -static_cast<LONGLONG>(delay), 0, NULL);',
                                    'ExSetTimer(timer, -static_cast<LONGLONG>(delay), 60606, NULL);'),
    'early-delivery': fixture.replace('if (due)', 'if (active)'),
    'rearm-after-stop': fixture.replace('InterlockedCompareExchange(&m_CrtcVsyncTimerArmed, 0, 0) != 0', 'true'),
    'foreign-timer': fixture.replace('timer == m_CrtcVsyncTimer', 'true'),
    'scanline-callback-phase': fixture.replace('if (armed)', 'if (false && armed)'),
}
with tempfile.TemporaryDirectory(prefix='vblank-clock-') as temp:
    path = Path(temp)
    for label, unit in controls.items():
        if label != 'production':
            assert unit != fixture, label
        (path / 'test.cpp').write_text(unit)
        subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-variable', '-pthread', '-fsanitize=address,undefined',
                        '-I' + str(gpu / 'common'), str(path / 'test.cpp'),
                        '-o', str(path / 'test')], check=True)
        result = subprocess.run([str(path / 'test')], capture_output=True, text=True)
        print(label, result.returncode, result.stdout[:400].strip(), result.stderr[:300])
        if label == 'production':
            assert result.returncode == 0
        else:
            assert result.returncode == 1 and 'FAIL' in result.stdout
