#!/usr/bin/env python3
"""Compile actual KMD notification functions with a portable object/lock shim."""
from pathlib import Path
import resource
import subprocess
import tempfile

resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
here = Path(__file__).resolve().parent
source = (here.parents[1] / 'viogpuwddm/wddmddi.cpp').read_text()

def function(name):
    start = source.index(name + '(')
    # Comments may name helpers before their definitions.
    while source[source.rfind('\n', 0, start) + 1:start].strip() not in ('VOID', 'NTSTATUS'):
        start = source.index(name + '(', start + 1)
    start = source.rfind('\n', 0, start) + 1
    brace = source.index('{', start)
    end, depth = brace + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

names = ['NotifyContextFenceWaitersLocked', 'ArmContextFenceEvent',
         'CancelContextFenceEvent', 'PublishContextCompletedUmdFence',
         'InvalidateContextUmdFenceTracker', 'BeginContextSubmissionRundown']
fixture = (here / 'test.cpp').read_text().replace('// PRODUCTION', '\n\n'.join(map(function, names)))
controls = {
    'production': fixture,
    'lost-inline-wake': fixture.replace('KeSetEvent(event, IO_NO_INCREMENT, FALSE);', ';'),
    'lost-event-reference': fixture.replace('ObDereferenceObjectDeferDelete(event);', ';'),
    'regressing-completion': fixture.replace('current == 0 || static_cast<INT32>(completed - current) > 0', 'true'),
    'stale-cookie-reuse': fixture.replace('++context->NextFenceWaitCookie', '1'),
    'accept-after-reset': fixture.replace('context->SubmissionClosing || context->FenceWaitInvalidated', 'context->SubmissionClosing'),
}
with tempfile.TemporaryDirectory(prefix='fence-events-') as temp:
    path = Path(temp)
    for label, unit in controls.items():
        if label != 'production':
            assert unit != fixture
        (path / 'test.cpp').write_text(unit)
        subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-variable', '-pthread', '-fsanitize=address,undefined',
                        str(path / 'test.cpp'), '-o', str(path / 'test')], check=True)
        result = subprocess.run([str(path / 'test')], capture_output=True, text=True)
        print(label, result.returncode, result.stdout.strip(), result.stderr[:400])
        assert result.returncode == (0 if label == 'production' else 1)
