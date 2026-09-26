#!/usr/bin/env python3
"""Compile the production surface registry against a deterministic host shim."""
from pathlib import Path
import argparse
import os
import re
try:
    import resource
except ImportError:  # Windows has no POSIX resource limits.
    resource = None
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--negative-controls', action='store_true')
args = parser.parse_args()
if resource is not None:
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
here = Path(__file__).resolve().parent
root = here.parents[2]
source = (root / 'viogpu/viogpuwddm/wddmddi.cpp').read_text()

def declaration(name, kind='function'):
    pattern = (r'^struct ' + name + r'\s*\{' if kind == 'struct' else
               r'^[^;\n]*\b' + name + r'\([^;]*?\)\s*\{')
    match = re.search(pattern, source, re.M)
    if not match:
        raise RuntimeError('missing production definition: ' + name)
    start = source.index('{', match.start())
    depth, end = 1, start + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end + (kind == 'struct')]

names = ['FindNativeShareByKeyLocked', 'CollectNativeSurfacesLocked', 'NativeHostSurfaceAllocationEligible',
         'QueryNativeScanoutProfileEscape', 'QueryNativeScanoutDiagnosticEscape', 'HandleNativeSurfaceEscape',
         'ReferenceHostSurfaceAllocation', 'ReleaseHostSurfaceAllocation', 'RemoveNativeImportsForContext',
         'ImportNativeShareLocked', 'ReleaseNativeShareLocked', 'VioGpuWddmRetireNativeShares',
         'IsOwnedAllocation', 'IsNativeAllocation', 'IsStandardAllocation', 'IsStandardPrimaryAllocation',
         'IsScanoutPrimaryAllocation', 'VioGpuWddmValidateDiagnosticPrimary',
         'BeginAllocationDestroy', 'UnmapHostSurfaceAllocation', 'VioGpuWddmDestroyAllocation']
production = '\n'.join(declaration(name) for name in names)
structs = '\n'.join(declaration(name, 'struct') for name in
                    ['VIOGPU_WDDM_NATIVE_SHARE_ENTRY', 'VIOGPU_WDDM_NATIVE_IMPORT_ENTRY'])
fixture = (here / 'native_surface_test.cpp').read_text().replace('// INSERT_STRUCTS', structs)
variants = [('production', production)]
if args.negative_controls:
    mutations = [
        ('foreign-free', 'share->OwnerProcess != PsGetCurrentProcess()', 'false'),
        ('live-import-free', 'share->ImportReferences,\n                                         share->AllocationReferences',
         '0, share->AllocationReferences'),
        ('stale-import', 'share->SurfaceResetGeneration != snapshot->ResetGeneration ||\n         share->Surface.ResetGeneration != snapshot->ResetGeneration', 'false'),
        ('forged-pitch', 'info->Pitch == share->Surface.Stride && resource->Stride == share->Surface.Stride', 'true'),
        ('wrapper-destroy-routing', 'else if (allocation->HostSurface)', 'else if (false)'),
        ('wrapper-destroy-live-owner',
         'allocation->SubmissionReferences != 0 || allocation->OpenReferences != 0', 'false'),
        ('profile-readiness', '!(candidate ? adapter->NativeScanoutDiagnosticEnabled() : adapter->NativeScanoutProfileReady()) ||', ''),
        ('profile-stale-allocation', 'NativeHostSurfaceAllocationEligible(share))',
         '(NativeHostSurfaceAllocationEligible(share) || true))'),
        ('candidate-lost-pending-bind', 'InterlockedExchange(&share->ScanoutBindPending, 1);', ''),
        ('profile-not-copied', '*binding = share->ScanoutBinding;', '(void)binding;'),
        ('host-flags-claim-ready', 'request.ReadyFlags = 0;', 'request.ReadyFlags = VIOGPU_WDDM_SCANOUT_READY_ALL;'),
        ('primary-wrong-width', 'allocation->Width == mode->Geometry.StorageWidth &&', ''),
        ('primary-wrong-mode', 'VioGpuScanoutBindingMatches(&allocation->ScanoutBinding,',
         '(true || VioGpuScanoutBindingMatches(&allocation->ScanoutBinding,'),
    ]
    for name, old, new in mutations:
        if production.count(old) != 1:
            raise RuntimeError('negative control anchor changed: ' + name)
        mutated = production.replace(old, new)
        if name == 'primary-wrong-mode':
            mutated = mutated.replace('allocation->Width, allocation->Height) &&', 'allocation->Width, allocation->Height)) &&')
        variants.append((name, mutated))
with tempfile.TemporaryDirectory(prefix='.native-surface-', dir=here) as temp:
    output = Path(temp)
    for name, body in variants:
        unit = output / (name + '.cpp')
        binary = output / name
        unit.write_text(fixture.replace('// INSERT_PRODUCTION', body))
        subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-fsanitize=undefined',
                        '-I' + str(root / 'viogpu/common'), str(unit), '-o', str(binary)],
                       env={**os.environ, 'TMPDIR': str(output)}, check=True)
        result = subprocess.run([str(binary)], cwd=output, capture_output=True, text=True)
        if (result.returncode == 0) != (name == 'production'):
            raise SystemExit(name + ': unexpected result\n' + result.stdout + result.stderr)
        print('PASS ' + name + (' rejected' if name != 'production' else ''))
    query_fixture = (here / 'resource_query_test.cpp').read_text()
    query = declaration('QueryNativeSurfaceResource')
    query_variants = [('production', query)]
    if args.negative_controls:
        for name, old, new in [
            ('foreign-process', 'share->OwnerProcess != PsGetCurrentProcess()', 'false'),
            ('wrong-parent', 'resource != allocation->Resource', 'false'),
            ('foreign-device', 'opened->Device->Adapter == adapter', 'true'),
            ('null-parent', 'if (parent == 0)', 'if (false)'),
            ('parent-under-allocation-pin',
             'stage = 6;\n        lookup.hObject = parent;',
             'stage = 6;\n        (void)dxgk->DxgkCbGetHandleParent(request.AllocationHandle);\n        lookup.hObject = parent;'),
        ]:
            if query.count(old) != 1:
                raise RuntimeError('query mutation anchor changed: ' + name)
            query_variants.append((name, query.replace(old, new)))
    for name, body in query_variants:
        unit, binary = output / ('query-' + name + '.cpp'), output / ('query-' + name)
        unit.write_text(query_fixture.replace('// INSERT_QUERY', body))
        subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-fsanitize=undefined',
                        '-I' + str(root / 'viogpu/shared'), str(unit), '-o', str(binary)],
                       env={**os.environ, 'TMPDIR': str(output)}, check=True)
        result = subprocess.run([str(binary)], cwd=output, capture_output=True, text=True)
        if (result.returncode == 0) != (name == 'production'):
            raise SystemExit('query-' + name + ': unexpected result\n' + result.stdout + result.stderr)
        print('PASS resource query ' + name + (' rejected' if name != 'production' else ''))
subprocess.run(['python3', str(here / 'paging_context.py')], check=True)
