#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Reject restricted-pool dependencies and media commands routed to GPU queues."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[3]
BLOCKED = re.compile(r'(?i)rdma[_-]?pool|rdmapool|droidvmpool|viogpu_named_pool|VIRTIO_GPU_CMD_')
paths = [ROOT / 'viogpu/viogpuvideo/driver.c', ROOT / 'viogpu/viogpuvideo/viogpuvideo.vcxproj',
         ROOT / 'viogpu/viogpuvideo/viogpuvideo.inf', ROOT / 'viogpu/video/install-video.ps1']
paths += list((ROOT / 'viogpu/video').glob('*.h'))
paths += list((ROOT / 'viogpu/video').glob('*.cpp'))
for path in paths:
    if BLOCKED.search(path.read_text(encoding='utf-8')):
        raise SystemExit('Unexpected dependency or GPU command in ' + str(path.relative_to(ROOT)))
print('PASS: video implementation has no restricted-pool or GPU commandq dependency')
