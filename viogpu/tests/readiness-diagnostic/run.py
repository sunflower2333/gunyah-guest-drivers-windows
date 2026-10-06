#!/usr/bin/env python3
"""Execute production destruction and first-reset capture with controlled OS peers."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--negative-controls', action='store_true')
args = parser.parse_args()
here = Path(__file__).resolve().parent
root = here.parents[2]
source = (root / 'viogpu/viogpudo/viogpudo.cpp').read_text()
header = (root / 'viogpu/viogpudo/viogpudo.h').read_text()
queue = (root / 'viogpu/common/viogpu_queue.h').read_text()

def span(text, first, after):
    begin = text.index(first)
    return text[begin:text.index(after, begin)]

definitions = span(header, 'enum VIOGPU_2D_DESTROY_FAILURE_STAGE', '/* Pack the two facts')
definitions += span(queue, 'enum VIOGPU_HOST_CONTEXT_RESULT', 'typedef VOID (*VIOGPU_NATIVE_AHB_COMPLETION)')
definitions += span(queue, 'enum VIOGPU_2D_RESOURCE_STATE', '#define MAX_INLINE_CMD_SIZE')
production = span(source, '__declspec(code_seg(".text")) VOID VioGpuDod::RecordFirstResetRequest', '#endif')
production += span(source, '__declspec(noinline) VOID VioGpuDod::RequestHardwareResetAtAnyIrql', 'VOID VioGpuDod::DpcRoutine')
production += span(source, 'VIOGPU_HOST_CONTEXT_RESULT VioGpuDod::Destroy2DResource', 'BOOLEAN VioGpuDod::Reconcile2DResourceAfterReset')
production += span(source, 'VIOGPU_HOST_CONTEXT_RESULT VioGpuAdapter::Destroy2DResource', 'BOOLEAN VioGpuAdapter::IsNativeContextResetRetired')
fixture = (here / 'readiness_diagnostic_test.cpp').read_text().replace('// INSERT_DEFINITIONS', definitions)
controls = [('production', production, None)]
if args.negative_controls:
    for field, message in [('m_ResetRequestPublication', 'first reset request is immutable'),
                           ('m_2DDestroyPublication', 'first destroy refusal is immutable')]:
        guard = f'if (InterlockedCompareExchange(&{field}, 1, 0) != 0)'
        assert production.count(guard) == 1
        controls.append((field, production.replace(guard, 'if (false)'), message))
    controls.append(('unmap-stage', production.replace('failureStage = VioGpu2DDestroyUnmap;',
                                                     'failureStage = VioGpu2DDestroyUnref;'),
                     'unmap failure is distinct and does not submit UNREF'))
    controls.append(('reset-order', production.replace('    RecordFirstResetRequest(callerRva);', '')
                     .replace('    LONG previousState = InterlockedExchange',
                              '    m_HardwareResetState = VioGpuHardwareResetRequested;\n    RecordFirstResetRequest(callerRva);\n    LONG previousState = InterlockedExchange'),
                     'first reset observation precedes the reset latch'))
with tempfile.TemporaryDirectory(prefix='viogpu-readiness-diagnostic-') as temporary:
    output = Path(temporary)
    for name, implementation, failure in controls:
        unit = output / f'{name}.cpp'
        unit.write_text(fixture.replace('// INSERT_PRODUCTION', implementation))
        binary = output / name
        if shutil.which('cl'):
            binary = binary.with_suffix('.exe')
            command = ['cl', '/nologo', '/EHsc', '/W4', '/WX', '/std:c++17', str(unit), f'/Fe{binary}']
        else:
            command = ['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pthread',
                       '-fsanitize=address,undefined', '-fno-omit-frame-pointer', str(unit), '-o', str(binary)]
        subprocess.run(command, cwd=output, check=True)
        result = subprocess.run([str(binary)], capture_output=True, text=True)
        if failure:
            if result.returncode != 1 or f'FAIL {failure}' not in result.stdout:
                raise SystemExit(f'Negative control failed: {name}\n{result.stdout}\n{result.stderr}')
            print(f'PASS negative control: {name}')
        else:
            print(result.stdout, end='')
            print(result.stderr, end='')
            if result.returncode:
                raise SystemExit(result.returncode)
