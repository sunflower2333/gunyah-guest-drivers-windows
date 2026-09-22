#!/usr/bin/env python3
"""Compile the production asynchronous AHB transport with a deterministic queue."""
from pathlib import Path
import argparse
import os
import re
import resource
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--negative-controls', action='store_true')
args = parser.parse_args()
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
here = Path(__file__).resolve().parent
root = here.parents[2]
source = (root / 'viogpu/common/viogpu_queue.cpp').read_text()

def definition(name, structure=False):
    pattern = (r'^struct ' + name + r'\s*\{' if structure else
               r'^[^;\n]*\b' + re.escape(name) + r'\([^;]*?\)\s*\{')
    match = re.search(pattern, source, re.M)
    if not match:
        raise RuntimeError('missing production definition: ' + name)
    start = source.index('{', match.start())
    depth, end = 1, start + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end + structure]

names = ['VioGpuArmVbufferTerminalCallbacks', 'VioGpuClaimVbufferTerminalCallbacks',
         'VioGpuDetachVbufferTerminalCallbacks', 'VioGpuCompleteVbufferTerminalCallbacks',
         'VioGpuSynchronousState', 'VioGpuReadSynchronousEpochState',
         'IsPlainControlResponse', 'IsPlainControlErrorResponse']
production = '\n'.join(definition(n) for n in names)
production += '\n' + definition('VIOGPU_NATIVE_AHB_PENDING', True)
production += '\n' + '\n'.join(definition('CtrlQueue::' + n) for n in
    ['CompleteNativeAhbOperation', 'CancelNativeAhbOperation', 'QueueNativeAhbOperation', 'PageNativeAhbSynchronous'])
header = (root / 'viogpu/common/viogpu.h').read_text()
wire = '\n'.join(re.search(r'typedef struct ' + n + r'\s*\{.*?\}[^;]+;', header, re.S)[0]
                 for n in ['virtio_gpu_ctrl_hdr', 'virtio_gpu_native_ahb_operation', 'virtio_gpu_native_ahb_paging'])
fixture = (here / 'native_ahb_release_test.cpp').read_text().replace('// INSERT_WIRE', wire)
variants = [('production', production)]
if args.negative_controls:
    for name, old, new in [
        ('short-response', 'buffer->response_size == sizeof(*response)', 'buffer->response_size >= sizeof(GPU_CTRL_HDR)'),
        ('stale-epoch', 'VioGpuReadSynchronousEpochState(&queue->m_SynchronousEpochState) == pending->Epoch', 'true'),
        ('wrong-sequence', 'response->sequence == pending->Sequence', 'true'),
        ('zero-present-sequence', 'response->sequence != 0', 'true'),
        ('wrong-resource', 'response->resource_id == pending->ResourceId', 'true'),
        ('cancel-grants-release', 'completion(callerContext, VioGpuHostContextUnknown, resourceId, sequence)',
         'completion(callerContext, VioGpuHostContextConfirmed, resourceId, sequence)'),
        ('paging-echo', 'response->offset == offset', 'true'),
        ('paging-short-read', 'buffer->response_size == responseSize', 'buffer->response_size >= sizeof(GPU_NATIVE_AHB_PAGING)'),
    ]:
        if production.count(old) != 1:
            raise RuntimeError('negative control anchor changed: ' + name)
        variants.append((name, production.replace(old, new)))
with tempfile.TemporaryDirectory(prefix='.native-ahb-release-', dir=here) as temp:
    output = Path(temp)
    for name, body in variants:
        unit, binary = output / (name + '.cpp'), output / name
        unit.write_text(fixture.replace('// INSERT_PRODUCTION', body))
        subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                        '-I' + str(root / 'viogpu/common'), str(unit), '-o', str(binary)],
                       env={**os.environ, 'TMPDIR': str(output)}, check=True)
        result = subprocess.run([str(binary)], cwd=output, capture_output=True, text=True)
        if (result.returncode == 0) != (name == 'production'):
            raise SystemExit(name + ': unexpected result\n' + result.stdout + result.stderr)
        print('PASS ' + name + (' rejected' if name != 'production' else ''))
