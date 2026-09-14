#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Compile actual scheduler/worker code in host fixtures, not a duplicate model."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]


def extract(text, signature):
    start = text.rindex(signature)
    brace = text.index('{', start)
    if ';' in text[start:brace]:
        raise ValueError(f'Not a definition: {signature}')
    end, depth = brace + 1, 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--negative-control-serial', action='store_true')
    parser.add_argument('--negative-control-worker-reference', action='store_true')
    parser.add_argument('--negative-control-pool-reset', action='store_true')
    args = parser.parse_args()
    source = (ROOT / 'viogpu/viogpudo/viogpudo.cpp').read_text()
    header = (ROOT / 'viogpu/viogpudo/viogpudo.h').read_text()
    wddm = (ROOT / 'viogpu/viogpuwddm/wddmddi.cpp').read_text()
    begin = header.index('typedef VOID (*VIOGPU_NATIVE_PASSIVE_ROUTINE)')
    end = header.index('\n#endif', header.index('struct VIOGPU_NATIVE_PASSIVE_WORK\n', begin))
    records = header[begin:end]
    methods = ['BOOLEAN VioGpuDod::QueueNativePassiveWork(',
               'BOOLEAN VioGpuDod::NativePassiveDispatchReadyLocked(',
               'BOOLEAN VioGpuDod::NativePassiveIdleLocked(',
               'VOID VioGpuDod::ReleaseNativePassiveDispatch(',
               'VOID VioGpuDod::CompleteNativePassiveWork(',
               'VIOGPU_NATIVE_PASSIVE_WORK_OWNERSHIP VioGpuDod::CancelNativePassiveWork(',
               'VOID VioGpuDod::CloseNativePassiveQueue(',
               'VOID VioGpuDod::RunNativePassiveWorker(',
               'VOID VioGpuDod::ReportNativeSubmitPerf(']
    production = '\n\n'.join(extract(source, method) for method in methods)
    worker = extract(wddm, 'VOID NativeRenderDispatchWorker(PVOID callbackContext)')
    worker += '\n' + extract(wddm, 'VOID NativeRenderDispatchCancelled(PVOID callbackContext)')
    if args.negative_control_serial:
        token = 'adapter->ReleaseNativePassiveDispatch(&submission->Work);'
        assert worker.count(token) == 1
        worker = worker.replace(token, '(void)0;')
    if args.negative_control_worker_reference:
        token = '!ReferenceRenderSubmission(submission)'
        assert worker.count(token) == 1
        worker = worker.replace(token, 'false')
    fixture = (HERE / 'pipeline_test.cpp').read_text().replace('// INSERT_RECORDS', records)
    fixture = fixture.replace('// INSERT_PRODUCTION', production + '\n' + worker)
    negative = args.negative_control_serial or args.negative_control_worker_reference
    if args.negative_control_pool_reset:
        run_pool(True)
        return
    with tempfile.TemporaryDirectory(prefix='viogpu-submit-perf-') as temp:
        out = Path(temp)
        unit = out / 'pipeline.cpp'
        unit.write_text(fixture)
        for window in ([64] if negative else [1, 32, 64, 128]):
            msvc = shutil.which('cl')
            exe = out / ('pipeline.exe' if msvc else 'pipeline')
            command = (['cl', '/nologo', '/EHsc', '/std:c++17', '/W4', '/WX',
                        f'/DVIOGPU_NATIVE_PIPELINE_WINDOW={window}', str(unit), f'/Fe:{exe}']
                       if msvc else ['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                     '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie',
                                     f'-DVIOGPU_NATIVE_PIPELINE_WINDOW={window}', str(unit), '-o', str(exe)])
            subprocess.run(command, cwd=out, check=True, timeout=120)
            result = subprocess.run([str(exe)], cwd=out, text=True, capture_output=True, timeout=60)
            if negative:
                expected = ('issued.size()' if args.negative_control_serial else 'heap-use-after-free')
                if result.returncode == 0 or expected not in result.stdout + result.stderr:
                    raise RuntimeError('Negative control did not fail for the intended reason: ' + result.stdout + result.stderr)
                print('PASS negative control detected:', 'serialization' if args.negative_control_serial else 'missing worker reference')
            else:
                print(result.stdout, end='')
                if result.returncode:
                    raise RuntimeError(result.stderr)
    if not negative:
        run_pool(False)


def run_pool(negative):
    header = (ROOT / 'viogpu/common/viogpu_queue.h').read_text()
    source = (ROOT / 'viogpu/common/viogpu_queue.cpp').read_text()
    records = header[header.index('typedef struct virtio_gpu_vbuffer'):header.index('BOOLEAN VioGpuArmVbufferTerminalCallbacks')]
    pool_class = header[header.index('class VioGpuBuf\n'):header.index('class VioGpuMemSegment\n')]
    pool_class = pool_class.replace('  private:', '  public:')
    methods = ['BOOLEAN VioGpuBuf::Init(', 'BOOLEAN VioGpuBuf::Close(', 'PGPU_VBUFFER VioGpuBuf::GetBuf(',
               'void VioGpuBuf::FreeBuf(', 'void VioGpuBuf::ReclaimBuffers(', 'VioGpuBuf::VioGpuBuf()',
               'VioGpuBuf::~VioGpuBuf()', 'PVOID VioGpuBuf::AllocateMemoryUninitialized(',
               'PVOID VioGpuBuf::AllocateMemory(', 'void VioGpuBuf::FreeMemory(']
    production = '\n\n'.join(extract(source, method) for method in methods)
    free = extract(source, 'void VioGpuBuf::FreeBuf(')
    if 'for (' in free or 'while (' in free:
        raise RuntimeError('VBUFFER hot-path free regressed to scanning')
    if negative:
        assert production.count('buffer->pool_in_use = FALSE;') == 2
        production = production.replace('buffer->pool_in_use = FALSE;', '(void)0;')
    fixture = (HERE / 'pool_test.cpp').read_text().replace('// INSERT_RECORDS', records)
    fixture = fixture.replace('// INSERT_CLASS', pool_class).replace('// INSERT_PRODUCTION', production)
    with tempfile.TemporaryDirectory(prefix='viogpu-pool-perf-') as temp:
        out = Path(temp)
        unit = out / 'pool.cpp'; unit.write_text(fixture)
        msvc = shutil.which('cl')
        exe = out / ('pool.exe' if msvc else 'pool')
        command = (['cl', '/nologo', '/EHsc', '/std:c++17', '/W4', '/WX', str(unit), f'/Fe:{exe}']
                   if msvc else ['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-no-pie',
                                 '-fsanitize=address,undefined', '-fno-omit-frame-pointer', str(unit), '-o', str(exe)])
        subprocess.run(command, cwd=out, check=True, timeout=120)
        result = subprocess.run([str(exe)], cwd=out, text=True, capture_output=True, timeout=60)
        if negative:
            if result.returncode == 0 or '!buffer->pool_in_use' not in result.stdout + result.stderr:
                raise RuntimeError('Pool negative control did not catch detached ownership: ' + result.stdout + result.stderr)
            print('PASS negative control detected: missing reset ownership revocation')
        else:
            print(result.stdout, end='')
            if result.returncode:
                raise RuntimeError(result.stderr)


if __name__ == '__main__':
    main()
