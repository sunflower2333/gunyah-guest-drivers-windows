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
adapter = (root / 'viogpu/viogpudo/viogpudo.cpp').read_text()

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

primary_name = 'VioGpuWddmValidateDiagnosticPrimary'
primary_definition = declaration(primary_name)
# Preserve actual linkage: extracting everything into one translation unit
# otherwise hides an anonymous-namespace definition from the real caller.
namespace_open = list(re.finditer(r'^namespace\s*\{', source, re.M))
namespace_close = list(re.finditer(r'^} // namespace$', source, re.M))
if len(namespace_open) != 1 or len(namespace_close) != 1:
    raise RuntimeError('primary linkage fixture needs updated namespace boundaries')
primary_position = source.index(primary_definition)
primary_internal = namespace_open[0].start() < primary_position < namespace_close[0].start()
primary_scoped = ('namespace {\n' + primary_definition + '\n}'
                  if primary_internal else primary_definition)
primary_declaration = re.search(r'^BOOLEAN ' + primary_name + r'\([^;]*\);', adapter, re.M)
if not primary_declaration:
    raise RuntimeError('missing adapter primary declaration')

names = ['FindNativeShareByKeyLocked', 'CollectNativeSurfacesLocked', 'NativeHostSurfaceAllocationEligible',
         'QueryNativeScanoutProfileEscape', 'QueryNativeScanoutDiagnosticEscape', 'HandleNativeSurfaceEscape',
         'ReferenceHostSurfaceAllocation', 'ReleaseHostSurfaceAllocation', 'RemoveNativeImportsForContext',
         'ImportNativeShareLocked', 'ReleaseNativeShareLocked', 'VioGpuWddmRetireNativeShares',
         'IsOwnedAllocation', 'IsNativeAllocation', 'IsStandardAllocation', 'IsStandardPrimaryAllocation',
         'IsScanoutPrimaryAllocation', 'VioGpuWddmValidateDiagnosticPrimary',
         'BeginAllocationDestroy', 'UnmapHostSurfaceAllocation', 'VioGpuWddmDestroyAllocation']
production = '\n'.join(primary_scoped if name == primary_name else declaration(name) for name in names)
structs = '\n'.join(declaration(name, 'struct') for name in
                    ['VIOGPU_WDDM_NATIVE_SHARE_ENTRY', 'VIOGPU_WDDM_NATIVE_IMPORT_ENTRY'])
fixture = (here / 'native_surface_test.cpp').read_text().replace('// INSERT_STRUCTS', structs)
fixture = fixture.replace('int main() {',
                          'void CheckPrimaryLinkage();\nint main() {\n    CheckPrimaryLinkage();')
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
    compiler = ['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-fsanitize=undefined',
                '-I' + str(root / 'viogpu/common')]
    compile_env = {**os.environ, 'TMPDIR': str(output)}
    caller = output / 'primary-caller.cpp'
    caller_object = output / 'primary-caller.o'
    caller.write_text('using BOOLEAN=bool; using HANDLE=void*; class VioGpuDod;\n'
                      'struct VIOGPU_NATIVE_SCANOUT_MODE;\n' + primary_declaration.group() +
                      '\nvoid CheckPrimaryLinkage() {\n    (void)' + primary_name +
                      '(nullptr, nullptr, nullptr);\n}\n')
    subprocess.run(compiler + ['-c', str(caller), '-o', str(caller_object)],
                   env=compile_env, check=True)
    # The historical internal-linkage definition must compile successfully,
    # then fail when linked against the separate caller's actual declaration.
    internal = output / 'primary-internal-linkage.cpp'
    internal_object = output / 'primary-internal-linkage.o'
    internal_body = production.replace(primary_scoped, 'namespace {\n' + primary_definition + '\n}')
    internal.write_text(fixture.replace('// INSERT_PRODUCTION', internal_body))
    subprocess.run(compiler + ['-c', str(internal), '-o', str(internal_object)],
                   env=compile_env, check=True)
    linked = subprocess.run(compiler + [str(internal_object), str(caller_object),
                            '-o', str(output / 'primary-internal-linkage')],
                            env=compile_env, capture_output=True, text=True)
    if linked.returncode == 0 or 'undefined reference' not in linked.stderr or primary_name not in linked.stderr:
        raise SystemExit('primary internal linkage was not rejected by the linker\n' + linked.stderr)
    print('PASS primary-internal-linkage rejected by separate-TU link')
    for name, body in variants:
        unit = output / (name + '.cpp')
        binary = output / name
        unit.write_text(fixture.replace('// INSERT_PRODUCTION', body))
        subprocess.run(compiler + [str(unit), str(caller_object), '-o', str(binary)],
                       env=compile_env, check=True)
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
