#!/usr/bin/env python3
"""Execute production PFN packing and direct control enqueue through 1 GiB backing."""
import argparse
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import time

parser = argparse.ArgumentParser()
parser.add_argument('--revision', help='Use older production source as a negative control')
parser.add_argument('--expect-old-limit', action='store_true', help='Confirm the pre-repair 64 MiB fragmentation limit')
parser.add_argument('--negative-controls', action='store_true', help='Reject the historical one-page paging READ cap')
args = parser.parse_args()
here = Path(__file__).resolve().parent
root = here.parents[2]

def source(path):
    if args.revision:
        return subprocess.check_output(['git', 'show', f'{args.revision}:{path}'], cwd=root, text=True)
    return (root / path).read_text()

def function(text, signature):
    start = text.rindex(signature)
    brace = text.index('{', start)
    if ';' in text[start:brace]:
        raise ValueError(f'Expected a definition, not a forward declaration: {signature}')
    depth = 1
    end = brace + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]

ddi = source('viogpu/viogpuwddm/wddmddi.cpp')
header = source('viogpu/common/viogpu_queue.h')
limit = re.search(r'^#define VIOGPU_MAX_BACKING_ENTRIES\s+(.+)$', header, re.M).group(0)
capacity = re.search(r'^#define VIOGPU_CONTROL_SG_CAPACITY\s+(.+)$', header, re.M)
capacity = capacity.group(0) if capacity else '#define VIOGPU_CONTROL_SG_CAPACITY 256U'
inline = re.search(r'^#define VIOGPU_CONTROL_INLINE_SG_CAPACITY\s+(.+)$', header, re.M)
inline = inline.group(0) if inline else '#define VIOGPU_CONTROL_INLINE_SG_CAPACITY 256U'
queue = source('viogpu/common/viogpu_queue.cpp')
wire = source('viogpu/common/viogpu_3d_wire.h')
gpu = source('viogpu/common/viogpu.h')
paging = 'GPU_NATIVE_AHB_PAGING' in queue
wire_types = ''
if paging:
    wire_types = '\n'.join(re.search(r'#pragma pack\(1\)\s*typedef struct ' + name +
                                     r'\s*\{.*?#pragma pack\(\)', gpu, re.S).group(0)
                           for name in ('virtio_gpu_ctrl_hdr', 'virtio_gpu_native_ahb_paging'))
    wire_types += '\n' + '\n'.join(re.findall(
        r'^#define (?:VIRTIO_GPU_CMD_PAGE_NATIVE_AHB|VIRTIO_GPU_NATIVE_AHB_PAGE_READ|'
        r'VIRTIO_GPU_NATIVE_AHB_PAGING_MAX_BYTES)\s+[^\n]+', wire, re.M))
    wire_types += '\n#define TEST_NATIVE_PAGING 1\n'
helpers = [function(queue, 'static UINT BuildSGElements(')]
if 'static UINT ControlDescriptorCount(' in queue:
    helpers += [function(queue, 'static UINT ControlDescriptorCount('),
                function(queue, 'static BOOLEAN BuildControlSG(')]
helpers += [function(queue, 'int CtrlQueue::QueueBuffer(')]
production = '\n'.join([wire_types, capacity, inline, limit,
    '#define SGLIST_SIZE VIOGPU_CONTROL_SG_CAPACITY'] + helpers +
    [function(ddi, signature) for signature in (
        'NTSTATUS BuildPfnEntries(', 'BOOLEAN ValidateAperturePageState(',
        'NTSTATUS AllocateApertureBackingEntries(')])
fixture = (here / 'backing_entries_test.cpp').read_text().replace('// INSERT_PRODUCTION', production)
with tempfile.TemporaryDirectory(prefix='viogpu-backing-entries-') as output:
    directory = Path(output)
    unit = directory / 'test.cpp'
    unit.write_text(fixture)
    if shutil.which('cl'):
        exe = directory / 'test.exe'
        command = ['cl', '/nologo', '/EHsc', '/W4', '/WX', '/std:c++17', str(unit), f'/Fe{exe}']
    else:
        exe = directory / 'test'
        command = ['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-Wno-multichar',
                   '-fsanitize=address,undefined', '-fno-omit-frame-pointer', str(unit), '-o', str(exe)]
    subprocess.run(command, cwd=directory, check=True)
    result = subprocess.run([str(exe)] + (['old'] if args.expect_old_limit else []), cwd=directory).returncode
    if result == 0 and args.negative_controls:
        anchor = 'buf->resp_size < 0 ||'
        if not paging or fixture.count(anchor) != 1:
            raise RuntimeError('missing paging response sizing negative control anchor')
        unit.write_text(fixture.replace(anchor, anchor + ' buf->resp_size > static_cast<int>(PAGE_SIZE) ||'))
        subprocess.run(command, cwd=directory, check=True)
        negative = subprocess.run([str(exe)], cwd=directory, capture_output=True, text=True)
        if negative.returncode == 0 or 'maximum paging READ reaches the production queue' not in negative.stdout:
            raise RuntimeError('historical one-page response cap did not fail the paging enqueue test')
        print('PASS negative control: historical one-page paging READ cap rejected')
    for attempt in range(21):
        try:
            exe.unlink()
            break
        except PermissionError:
            if attempt == 20:
                raise
            time.sleep(0.25)
    raise SystemExit(result)
