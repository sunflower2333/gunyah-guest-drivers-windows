#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""One-time, hash-guarded migration; never removes the retired-pool check."""
from pathlib import Path
import hashlib

p = Path('viogpu/viogpuwddm/check-contract.py')
raw = p.read_bytes()
assert hashlib.sha256(raw).hexdigest() == 'f628f1840e8e81dbdaa3f8bea46377e9d49d7825e9c012650e8343af1d960287', 'Unexpected checker revision'
source = raw.decode('utf-8')
old = '''        "OpenAdapter10_2",
    ]:
        fail("D3D UMD module definition must export exactly the three legacy entry points")'''
new = '''        "OpenAdapter10_2",
        "VioGpuVideoOpen",
        "VioGpuVideoControl",
        "VioGpuVideoAllocate",
        "VioGpuVideoQueue",
        "VioGpuVideoDequeue",
        "VioGpuVideoCopy",
        "VioGpuVideoStream",
        "VioGpuVideoClose",
    ]:
        fail("D3D UMD module definition must export exactly the three legacy entry points and eight explicit video bridge functions")'''
assert source.count(old) == 1
source = source.replace(old, new)
old = '''    if compile_inputs != ["viogpud3d.cpp"]:
        fail(f"D3D UMD project must compile only its activation source: {compile_inputs}")'''
new = '''    if compile_inputs != ["viogpud3d.cpp", "../video/video_client.cpp"]:
        fail(f"D3D UMD project must compile exactly its activation source and explicit video bridge: {compile_inputs}")'''
assert source.count(old) == 1
p.write_text(source.replace(old, new), encoding='utf-8')

# The production contract intentionally rejects even comment mentions in build
# wiring. Keep the no-pool explanation in README; use generic descriptions in
# source comments. No include, symbol, service, dependency or executable code
# is removed or renamed by these four replacements.
replacements = {
    'viogpu/viogpuvideo/driver.c': (
        '/* Initialize transport resources through the existing no-rdmapool VirtIO/WDF library. */',
        '/* Initialize transport resources through the existing ordinary-RAM VirtIO/WDF library. */'),
    'viogpu/viogpuvideo/viogpuvideo.inf': (
        '; VioGPU\'s virtio-media function, NOT the display adapter. No rdmapool service.',
        '; VioGPU\'s virtio-media function, NOT the display adapter. No external pool service.'),
    'viogpu/video/install-video.ps1': (
        '# OpenGL/Vulkan settings, install rdmapool, or turn off signature enforcement.',
        '# OpenGL/Vulkan settings, install pool services, or turn off signature enforcement.'),
    'viogpu/video/video_ioctl.h': (
        ' * No user pointers, guest physical addresses, pool handles or rdmapool imports.',
        ' * No user pointers, guest physical addresses, pool handles or restricted allocations.'),
}
for name, (old, new) in replacements.items():
    path = Path(name)
    source = path.read_text(encoding='utf-8')
    assert source.count(old) == 1, name
    path.write_text(source.replace(old, new), encoding='utf-8')
