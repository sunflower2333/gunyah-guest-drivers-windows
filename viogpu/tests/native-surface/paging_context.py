#!/usr/bin/env python3
"""Execute the production paging Patch, Submit and Cancel branches."""
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
header = (root / 'viogpu/viogpuwddm/wddmddi.h').read_text()

def block(text, start):
    brace = text.index('{', start)
    end, depth = brace + 1, 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]

def definition(name):
    match = re.search(r'^[^;\n]*\b' + name + r'\([^;]*?\)\s*\{', source, re.M)
    if not match:
        raise RuntimeError(name)
    return block(source, match.start())

records = header[header.index('struct VIOGPU_WDDM_KMD_DMA_PRIVATE'):header.index('struct VIOGPU_WDDM_PRESENT_DMA_PACKET')]
records += header[header.index('enum VIOGPU_WDDM_PAGING_TRANSACTION_STATE'):header.index('static_assert(FIELD_OFFSET(VIOGPU_WDDM_PAGING_PRIVATE')]
records += block(source, source.index('struct VIOGPU_NATIVE_HOST_PAGING_WORK\n')) + ';'
production = '\n'.join(definition(n) for n in ['ValidatePagingDmaPacket', 'ResolvePagingBatchOffset',
    'ResolvePagingBatch', 'ValidatePagingTransactionReference', 'ReferencePagingContext',
    'ReleasePagingTransactionReference', 'CancelPagingTransaction', 'IsRecognizedPagingOwner',
    'CancelRecognizedPagingTransaction', 'CaptureNativeHostPagingOwners',
    'RunNativeHostPagingWork', 'DetachNativeHostPagingBatch'])
cancel = definition('VioGpuWddmCancelCommand')
start = cancel.index('else if (cancelCommand->hContext != NULL')
unrelated = block(cancel, start)
cancel = cancel.replace(unrelated, 'else {}')
production += '\n' + cancel
patch = definition('VioGpuWddmPatch')
patch = block(patch, patch.index('if (patchArguments->Flags.Value == 1)'))
patch = patch[patch.index('{')+1:-1]
submit = definition('VioGpuWddmSubmitCommand')
submit = block(submit, submit.index('if (pagingSubmission)', submit.index('NTSTATUS status = STATUS_SUCCESS;')))
submit = submit[submit.index('{')+1:-1]
fixture = (here / 'paging_context_test.cpp').read_text().replace('// INSERT_RECORDS', records)
fixture = fixture.replace('// INSERT_PRODUCTION', production).replace('// INSERT_PATCH', patch).replace('// INSERT_SUBMIT', submit)
variants = [('production', fixture)]
for name, old, new in [
    ('null-context-rejected', 'if (handle == NULL)\n        return TRUE;', 'if (handle == NULL)\n        return FALSE;'),
    ('foreign-context', 'context->Device->Adapter != adapter', 'false'),
    ('cancel-nonnull-paging', 'if (pagingOwner && pagingContextValid &&', 'if (cancelCommand->hContext == NULL && pagingOwner && pagingContextValid &&'),
    ('paging-owner-publication', 'first->Work.OrderingOwnerCount = ownerCount;', 'first->Work.OrderingOwnerCount = 0;'),
]:
    if fixture.count(old) != 1:
        raise RuntimeError('mutation anchor: ' + name)
    variants.append((name, fixture.replace(old, new)))
with tempfile.TemporaryDirectory(prefix='.paging-context-', dir=here) as temp:
    out = Path(temp)
    for name, text in variants:
        unit, binary = out / (name + '.cpp'), out / name
        unit.write_text(text)
        subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-no-pie',
                        str(unit), '-o', str(binary)], env={**os.environ, 'TMPDIR': str(out)}, check=True)
        result = subprocess.run([str(binary)], capture_output=True, text=True,
                                env={**os.environ, 'UBSAN_OPTIONS': 'halt_on_error=1'})
        if (result.returncode == 0) != (name == 'production'):
            raise SystemExit(name + ': unexpected result\n' + result.stdout + result.stderr)
        print('PASS paging context ' + name + (' rejected' if name != 'production' else ''))
