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
               r'^(?:static )?(?:VOID|BOOLEAN|NTSTATUS|VIOGPU_WDDM_NATIVE_SHARE_ENTRY\s*\*|VIOGPU_WDDM_ALLOCATION\s*\*)\s*' +
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
                   ['VIOGPU_WDDM_NATIVE_SHARE_ENTRY', 'VIOGPU_WDDM_NATIVE_IMPORT_ENTRY',
                    'VIOGPU_NATIVE_AHB_PRESENT_COMPLETION', 'VIOGPU_NATIVE_POISON_SAMPLE',
                    'VIOGPU_NATIVE_POISON_RECORD', 'VIOGPU_HOST_SURFACE_PAGING_FAILURE'])
structs += '''
VIOGPU_NATIVE_POISON_RECORD g_VioGpuNativePoisonRecord;
VIOGPU_HOST_SURFACE_PAGING_FAILURE g_VioGpuHostSurfacePagingFailure;'''
production = '\n'.join(definition(n) for n in
    ['FindNativeShareByKeyLocked', 'RecordNativePoisonLocked', 'NativeAhbReleaseObserved',
     'ResolveKeyedHostSurfaceAllocation', 'ResolveHostImportAllocation', 'PinNativeSubmitImports',
     'ReleasePendingSurfaceWriterLocked',
     'UnpinNativeSubmitImports', 'AdmitNativeSubmitImports', 'RetireNativeSubmitImports',
     'RepackNativeSubmitImports', 'PublishStandardPlacement', 'ClearNativePlacement', 'ExecuteHostSurfacePaging',
     'NativeAhbPresentAccepted', 'PresentHostSurface', 'PresentResidentHostSurface'])
production += '\n' + definition('VioGpuDod::NativePassiveDispatchReadyLocked', adapter)
production += '\n' + definition('VioGpuDod::TryResumeNativePassiveDispatch', adapter)
fixture = (here / 'ownership_test.cpp').read_text().replace('// INSERT_STRUCTS', structs)
variants = [('production', production)]
for name, old, new in [
    ('forged-iova', 'candidate->Iova == ref.Iova', 'true'),
    ('present-before-rendered-write', 'if (share->PendingSurfaceWriters == 0)', 'if (true)'),
    ('pending-writer-double-release', 'if (!import->PendingWriterCounted)\n        return;', 'if (false)\n        return;'),
    ('early-retirement', 'submission->ImportsHostIssued && !confirmed', 'false'),
    ('sequence-release', 'share->Access.ReleasedSequence = sequence;', 'share->Access.ReleasedSequence = 0;'),
    ('present-deadlock', 'InterlockedCompareExchange(&work->DisplayReleaseWait, 0, 0) == 0', '(work != nullptr)'),
    ('same-context-reorder', 'status = STATUS_PENDING;\n    }\n    KeReleaseSpinLockFromDpcLevel',
     'status = STATUS_SUCCESS;\n    }\n    KeReleaseSpinLockFromDpcLevel'),
    ('missing-residency', 'bos[index].Handle = share->ResourceId;', 'bos[index].Handle = 0;'),
    ('evicted-render', '!share->SurfaceResident || allocation == NULL', 'false || allocation == NULL'),
    ('forged-vidmm-allocation', 'allocation->ShareKey != ref.ShareKey || allocation->PrivateData.Size', 'false || allocation->PrivateData.Size'),
    ('paging-error-reuse', 'share->Access.Poisoned = true;\n        share->Access.Writer = false;',
     'share->Access.Poisoned = false;\n        share->Access.Writer = false;'),
    ('paging-omit-transfer', 'while (status == STATUS_SUCCESS && !bookkeepingOnly && completed < transaction->TransferSize)',
     'while (false && status == STATUS_SUCCESS && !bookkeepingOnly && completed < transaction->TransferSize)'),
    ('present-paging-mutex-cycle',
     'KeReleaseMutex(&allocation->LifecycleMutex, FALSE);\n    if (status == STATUS_SUCCESS)\n    {\n        status = PresentHostSurface(adapter, allocation);',
     'if (status == STATUS_SUCCESS)\n    {\n        status = PresentHostSurface(adapter, allocation);\n        KeReleaseMutex(&allocation->LifecycleMutex, FALSE);'),
    ('present-evicted-after-wait', 'BOOLEAN idle = share->SurfaceResident && !share->Access.Poisoned &&',
     'BOOLEAN idle = !share->Access.Poisoned &&'),
    ('paging-positive-wait',
     'return status != STATUS_SUCCESS && NT_SUCCESS(status) ? STATUS_DEVICE_NOT_READY : status;', 'return status;'),
    ('present-front-repeat', 'const BOOLEAN front = usable && held && adapter->IsActiveScanoutResource(share->ResourceId);',
     'const BOOLEAN front = false;'),
    ('present-held-no-release', 'else if (usable && !held && !share->Access.WaitPending)',
     'else if (usable && !share->Access.WaitPending)'),
    ('paging-worker-reorder', '!orderingConflict && !m_NativePassiveClosing', '(orderingConflict || !orderingConflict) && !m_NativePassiveClosing'),
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
        result = subprocess.run([str(binary)], capture_output=True, text=True,
                                env={**os.environ, 'UBSAN_OPTIONS': 'halt_on_error=1'})
        if (result.returncode == 0) != (name == 'production'):
            raise SystemExit(name + ': unexpected result\n' + result.stdout + result.stderr)
        print('PASS ownership ' + name + (' rejected' if name != 'production' else ''))
