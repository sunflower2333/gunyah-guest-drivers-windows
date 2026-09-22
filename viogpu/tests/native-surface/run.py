#!/usr/bin/env python3
"""Compile the production surface registry against a deterministic host shim."""
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

names = ['FindNativeShareByKeyLocked', 'CollectNativeSurfacesLocked', 'HandleNativeSurfaceEscape',
         'ReferenceHostSurfaceAllocation', 'ReleaseHostSurfaceAllocation', 'RemoveNativeImportsForContext',
         'ImportNativeShareLocked', 'ReleaseNativeShareLocked', 'VioGpuWddmRetireNativeShares']
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
    ]
    for name, old, new in mutations:
        if production.count(old) != 1:
            raise RuntimeError('negative control anchor changed: ' + name)
        variants.append((name, production.replace(old, new)))
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
