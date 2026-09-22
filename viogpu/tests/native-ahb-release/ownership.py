#!/usr/bin/env python3
"""Exercise production import admission, retirement, packet repack and barriers."""
from pathlib import Path
import os
import re
import resource
import subprocess
import tempfile

resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
here = Path(__file__).resolve().parent
root = here.parents[2]
source = (root / 'viogpu/viogpuwddm/wddmddi.cpp').read_text()
adapter = (root / 'viogpu/viogpudo/viogpudo.cpp').read_text()

def definition(name, text=source, structure=False):
    pattern = (r'^struct ' + name + r'\s*\{' if structure else
               r'^(?:static )?(?:VOID|BOOLEAN|NTSTATUS|VIOGPU_WDDM_NATIVE_SHARE_ENTRY\s*\*)\s*' +
               re.escape(name) + r'\([^;]*?\)\s*\{')
    match = re.search(pattern, text, re.M)
    if not match:
        raise RuntimeError('missing production definition: ' + name)
    start = text.index('{', match.start())
    depth, end = 1, start + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[match.start():end + structure]

structs = '\n'.join(definition(n, structure=True) for n in
                   ['VIOGPU_WDDM_NATIVE_SHARE_ENTRY', 'VIOGPU_WDDM_NATIVE_IMPORT_ENTRY'])
production = '\n'.join(definition(n) for n in
    ['FindNativeShareByKeyLocked', 'NativeAhbReleaseObserved', 'PinNativeSubmitImports',
     'UnpinNativeSubmitImports', 'AdmitNativeSubmitImports', 'RetireNativeSubmitImports',
     'RepackNativeSubmitImports'])
production += '\n' + definition('VioGpuDod::NativePassiveDispatchReadyLocked', adapter)
fixture = (here / 'ownership_test.cpp').read_text().replace('// INSERT_STRUCTS', structs)
variants = [('production', production)]
for name, old, new in [
    ('forged-iova', 'candidate->Iova == ref.Iova', 'true'),
    ('early-retirement', 'submission->ImportsHostIssued && !confirmed', 'false'),
    ('sequence-release', 'share->Access.ReleasedSequence = sequence;', 'share->Access.ReleasedSequence = 0;'),
    ('present-deadlock', 'InterlockedCompareExchange(&work->DisplayReleaseWait, 0, 0) == 0', '(work != nullptr)'),
    ('same-context-reorder', 'status = STATUS_PENDING;\n    }\n    KeReleaseSpinLockFromDpcLevel',
     'status = STATUS_SUCCESS;\n    }\n    KeReleaseSpinLockFromDpcLevel'),
    ('missing-residency', 'bos[index].Handle = share->ResourceId;', 'bos[index].Handle = 0;'),
]:
    if production.count(old) != 1:
        raise RuntimeError('mutation anchor changed: ' + name)
    variants.append((name, production.replace(old, new)))
with tempfile.TemporaryDirectory(prefix='.native-ahb-ownership-', dir=here) as temp:
    output = Path(temp)
    for name, body in variants:
        unit, binary = output / (name + '.cpp'), output / name
        unit.write_text(fixture.replace('// INSERT_PRODUCTION', body))
        subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                        '-I' + str(root / 'viogpu/common'), str(unit), '-o', str(binary)],
                       env={**os.environ, 'TMPDIR': str(output)}, check=True)
        result = subprocess.run([str(binary)], capture_output=True, text=True)
        if (result.returncode == 0) != (name == 'production'):
            raise SystemExit(name + ': unexpected result\n' + result.stdout + result.stderr)
        print('PASS ownership ' + name + (' rejected' if name != 'production' else ''))
