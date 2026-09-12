#!/usr/bin/env python3
"""Run production queue wait/poison/recovery and timeout decoding with OS peers."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

parser = argparse.ArgumentParser()
parser.add_argument('--negative-control-overwrite', action='store_true')
parser.add_argument('--negative-control-mode-publication', action='store_true')
args = parser.parse_args()
here = Path(__file__).resolve().parent
root = here.parents[2]
queue = (root / 'viogpu/common/viogpu_queue.cpp').read_text()
header = (root / 'viogpu/common/viogpu_queue.h').read_text()
wire = (root / 'viogpu/common/viogpu.h').read_text()
dod = (root / 'viogpu/viogpudo/viogpudo.cpp').read_text()

def span(source, first, after):
    begin = source.index(first)
    return source[begin:source.index(after, begin)]

definitions = span(wire, '#pragma pack(1)\ntypedef struct virtio_gpu_rect',
                   '/* VIRTIO_GPU_RESP_OK_DISPLAY_INFO */')
definitions += span(header, 'typedef struct virtio_gpu_vbuffer', '// #pragma pack()')
definitions += span(header, 'enum VIOGPU_SYNCHRONOUS_STATE', 'enum VIOGPU_HOST_CONTEXT_RESULT')
definitions += span(header, 'enum VIOGPU_HOST_CONTEXT_RESULT', '/* Captures the control-queue response')
definitions += span(header, 'enum VIOGPU_2D_RESOURCE_STATE', '#define MAX_INLINE_CMD_SIZE')
production = span(queue, 'static LONG64 VioGpuMakeSynchronousEpochState', 'static BOOLEAN IsPlainControlResponse')
production += span(queue, 'static void VioGpuDecodeSynchronousTimeoutCommand', 'NTSTATUS CtrlQueue::QuiesceSynchronousRequests')
production += span(queue, 'void CtrlQueue::CompleteSynchronousRequestTeardown', 'PAGED_CODE_SEG_BEGIN')
production += span(queue, 'BOOLEAN CtrlQueue::SubmitSynchronousLocked(PGPU_VBUFFER buf, _Out_ PBOOLEAN release_buffer)\n',
                   'VIOGPU_HOST_CONTEXT_RESULT CtrlQueue::SubmitSynchronousNoDataLocked')
production += span(queue, 'ULONG CtrlQueue::SynchronousPoisonCallerRva', 'static void VioGpuDecodeSynchronousTimeoutCommand')
if args.negative_control_overwrite:
    guard = 'if (InterlockedCompareExchange(&m_SynchronousTimeoutPublication, 1, 0) != 0)'
    assert production.count(guard) == 1
    production = production.replace(guard, 'if (false)')
fixture = (here / 'synchronous_timeout_test.cpp').read_text()
fixture = fixture.replace('// INSERT_DEFINITIONS', definitions).replace('// INSERT_PRODUCTION', production)
mode_production = span(dod, 'VIOGPU_HOST_CONTEXT_RESULT VioGpuDod::Flush2DResource(',
                       'NTSTATUS VioGpuDod::PublishPresentBlit(')
mode_production += span(dod, 'VIOGPU_HOST_CONTEXT_RESULT VioGpuAdapter::Flush2DResource(',
                        'NTSTATUS VioGpuAdapter::PublishPresentBlit(')
mode_production += span(dod, 'VOID VioGpuDod::RecordNativeSynchronousPoisonDiagnostic(',
                        'VOID VioGpuDod::RecordNativeReadinessDiagnostic(')
if args.negative_control_mode_publication:
    call = 'RecordNativeSynchronousFailureDiagnostic();'
    assert mode_production.count(call) == 2
    mode_production = mode_production.replace(call, '(void)0;')
mode_fixture = (here / 'mode_publication_test.inc').read_text().replace('// INSERT_MODE_PRODUCTION', mode_production)
fixture = fixture.replace('// INSERT_MODE_FIXTURE', mode_fixture)
with tempfile.TemporaryDirectory(prefix='viogpu-synchronous-timeout-') as temporary:
    output = Path(temporary)
    unit = output / 'test.cpp'
    unit.write_text(fixture)
    include = str(root / 'viogpu/common')
    if shutil.which('cl'):
        binary = output / 'test.exe'
        command = ['cl', '/nologo', '/EHsc', '/W4', '/WX', '/std:c++17', f'/I{include}', str(unit), f'/Fe{binary}']
    else:
        binary = output / 'test'
        command = ['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pthread',
                   '-fsanitize=address,undefined', '-fno-omit-frame-pointer', f'-I{include}', str(unit), '-o', str(binary)]
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
    if args.negative_control_mode_publication:
        if result.returncode != 1 or 'FAIL mode flush failure publishes first timeout without context destruction' not in result.stdout:
            raise SystemExit('Negative control did not detect missing mode publication')
        print('PASS negative control: missing mode failure publication detected')
    elif args.negative_control_overwrite:
        if result.returncode != 1 or 'FAIL first submitted failure retained after recovery' not in result.stdout:
            raise SystemExit('Negative control did not detect first-failure overwrite')
        print('PASS negative control: first-failure overwrite detected')
    else:
        raise SystemExit(result.returncode)
