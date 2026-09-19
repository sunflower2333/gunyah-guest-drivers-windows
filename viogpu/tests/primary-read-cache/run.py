#!/usr/bin/env python3
"""Exercise the production cache, paging invalidation and Present row loop."""
from pathlib import Path
import subprocess
import tempfile

here = Path(__file__).resolve().parent
source = (here.parents[1] / 'viogpuwddm/wddmddi.cpp').read_text()

def function(name):
    start = source.index(name + '(')
    start = source.rfind('\n', 0, start) + 1
    brace = source.index('{', start)
    end, depth = brace + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

names = ['InvalidatePrimaryReadCache', 'ReleasePrimaryReadCache',
         'CopyAperturePlacement', 'FillAperturePlacement', 'CopyPresentRow',
         'PresentReadAddress', 'UpdatePrimaryReadCache']
parts = '\n\n'.join(function(name) for name in names)
execute = function('ExecutePresentTransaction')
start = execute.index('sourceBase = PresentReadAddress(source);')
end = execute.index('UpdatePrimaryReadCache(transaction, sourceBase);', start)
end += len('UpdatePrimaryReadCache(transaction, sourceBase);')
copy = execute[start:end]
# Cache validity must not survive replacement/unmapping of the backing pages.
assert 'ReleasePrimaryReadCache(allocation);' in function('ReleaseApertureCpuMapping')
assert 'ReleaseApertureCpuMapping(allocation);' in function('UnmapApertureAllocation')
fixture = (here / 'test.cpp').read_text()
fixture = fixture.replace('// PRODUCTION_HELPERS', parts).replace('// PRODUCTION_COPY', copy)
controls = {
    'current': fixture,
    'missing-paging-invalidation': fixture.replace(
        'InvalidatePrimaryReadCache(allocation);\n        RtlCopyMemory(apertureAddress',
        'RtlCopyMemory(apertureAddress'),
    'missing-fill-invalidation': fixture.replace(
        'InvalidatePrimaryReadCache(allocation);\n    for (SIZE_T offset',
        'for (SIZE_T offset'),
    'missing-reset-check': fixture.replace(
        'allocation->PrimaryReadCacheGeneration == allocation->Resource2DResetGeneration', 'true'),
}
for label, unit in controls.items():
    if label != 'current':
        assert unit != fixture, label
    with tempfile.TemporaryDirectory(prefix='primary-cache-') as temporary:
        path = Path(temporary)
        (path / 'test.cpp').write_text(unit)
        subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-Wno-multichar', '-fsanitize=address,undefined',
                        str(path / 'test.cpp'), '-o', str(path / 'test')], check=True)
        result = subprocess.run([str(path / 'test')], capture_output=True, text=True)
        print(label, 'exit', result.returncode, result.stdout.strip(), result.stderr.strip())
        if label == 'current':
            assert result.returncode == 0
        else:
            assert result.returncode == 1 and 'FAIL' in result.stdout
