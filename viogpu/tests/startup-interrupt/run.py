#!/usr/bin/env python3
"""Exercise actual startup/ISR/worker code with an inline DRIVER_OK interrupt."""
from pathlib import Path
import os
import subprocess
import tempfile

here = Path(__file__).resolve().parent
source = (here.parents[2] / 'viogpu/viogpudo/viogpudo.cpp').read_text()


def function(signature):
    start = source.index(signature)
    brace = source.index('{', start)
    end, depth = brace + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


initialization = function('NTSTATUS VioGpuAdapter::StartNativeContextTransport(')
start = initialization.index('    VIOGPU_RECORD_NATIVE_START(m_pVioGpuDod,\n                               VioGpuNativeStartQueueInterrupts,')
end = initialization.index('    status = ProbeNativeContextReadiness();', start)
startup = ('NTSTATUS VioGpuAdapter::InitializeControlTransport() {\n' +
           initialization[start:end] + '\nreturn STATUS_SUCCESS;\n}')
isr = function('BOOLEAN VioGpuAdapter::InterruptRoutine(')
worker = function('NTSTATUS VioGpuAdapter::StartWorkThread(')
gate = '    InterlockedExchange(&m_InterruptDispatchEnabled, TRUE);'
assert startup.count(gate) == 1
assert worker.count('    m_bStopWorkThread = FALSE;') == 1
late_gate = startup.replace(gate, '').replace('    m_pVioGpuDod->SetHardwareInit(TRUE);',
    '    m_pVioGpuDod->SetHardwareInit(TRUE);\n' + gate)
lost_event = worker.replace('    m_bStopWorkThread = FALSE;',
    '    m_bStopWorkThread = FALSE;\n    KeClearEvent(&m_ConfigUpdateEvent);')
fixture = (here / 'startup_interrupt_test.cpp').read_text()
with tempfile.TemporaryDirectory(prefix='.startup-irq-', dir=here) as temporary:
    output = Path(temporary)
    for name, init, work in (
        ('production', startup, worker),
        ('late-interrupt-gate', late_gate, worker),
        ('lost-config-event', startup, lost_event),
    ):
        unit, binary = output / (name + '.cpp'), output / name
        unit.write_text(fixture.replace('// INSERT_PRODUCTION', '\n'.join((init, isr, work))))
        subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        str(unit), '-o', str(binary)], check=True,
                       env={**os.environ, 'TMPDIR': temporary})
        result = subprocess.run([str(binary)], capture_output=True, text=True)
        if name == 'production':
            print(result.stdout, end='')
            print(result.stderr, end='')
            result.check_returncode()
        elif result.returncode != 2 or 'FAIL:' not in result.stderr:
            raise SystemExit(name + ': negative control did not fail semantically\n' + result.stderr)
        else:
            print('PASS negative control ' + name + ': ' + result.stderr.strip())
