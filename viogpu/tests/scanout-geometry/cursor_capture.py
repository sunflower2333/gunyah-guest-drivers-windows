#!/usr/bin/env python3
"""Run production cursor capture/reset bodies with host synchronization stubs."""
from pathlib import Path
import re
import subprocess
import tempfile

here = Path(__file__).resolve().parent
root = here.parents[2]
source = (root / 'viogpu/viogpudo/viogpudo.cpp').read_text()
header = (root / 'viogpu/viogpudo/viogpudo.h').read_text()


def body(name):
    match = re.search(r'^VOID VioGpuDod::' + name + r'\([^;]*?\)\s*\{', source, re.M)
    assert match, name
    start = source.index('{', match.start())
    depth, end = 1, start + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]


production = body('RecordNativeDiagnosticDdi') + '\n' + body('RecordNativeDiagnosticCursor')
members = header[header.index('    KMUTEX m_NativeDiagnosticCaptureMutex;'):
                 header.index('    volatile LONG m_UmdPresentActive;')]
start = source.index('    KeInitializeMutex(&m_NativeDiagnosticCaptureMutex, 0);')
initialization = source[start:source.index('    RtlZeroMemory((void *)m_NativeShareOk', start)]
fixture = (here / 'cursor_capture_test.cpp').read_text()
fixture = fixture.replace('// INSERT_MEMBERS', members).replace('// INSERT_INIT', initialization)
fast = '    if ((InterlockedCompareExchange(&m_NativeDiagnosticCursorMask, 0, 0) & bit) != 0) return;'
recheck = 'if ((InterlockedCompareExchange(&m_NativeDiagnosticCursorMask, 0, 0) & bit) == 0)'
rearm = '        InterlockedExchange(&m_NativeDiagnosticCursorMask, 0);'
zero = '        RtlZeroMemory(m_NativeDiagnosticCursorCapture, sizeof(m_NativeDiagnosticCursorCapture));'
for token in (fast, recheck, rearm, zero):
    assert production.count(token) == 1, token
variants = [
    ('production', production, None),
    ('no-fast-path', production.replace(fast, ''), 'duplicate position waited'),
    ('no-locked-recheck', production.replace(recheck, 'if (true)'), 'first cursor record overwritten'),
    ('no-rearm', production.replace(rearm, ''), 'new generation did not rearm'),
    ('early-rearm', production.replace(rearm, '').replace(zero, rearm + '\n' + zero),
     'rearm published before records cleared'),
]
with tempfile.TemporaryDirectory(prefix='.cursor-capture-', dir=here) as temporary:
    output = Path(temporary)
    for name, code, expected in variants:
        unit, binary = output / (name + '.cpp'), output / name
        unit.write_text(fixture.replace('// INSERT_PRODUCTION', code))
        subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pthread',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        '-I' + str(root / 'viogpu/common'), str(unit), '-o', str(binary)], check=True)
        result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=20)
        if expected is None:
            if result.returncode != 0:
                raise SystemExit(result.stdout + result.stderr)
            print(result.stdout.strip())
        else:
            if result.returncode != 1 or expected not in result.stderr:
                raise SystemExit(name + ': unexpected result\n' + result.stdout + result.stderr)
            print('PASS negative control ' + name + ': ' + expected)
