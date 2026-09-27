#!/usr/bin/env python3
"""Compile actual paging bodies/attributes into ARM64 COFF, including negatives."""
from pathlib import Path
import re
import struct
import subprocess
import tempfile
from verify_binary import REQUIRED

here = Path(__file__).resolve().parent
root = here.parents[2]
source = (root / 'viogpu/viogpuwddm/wddmddi.cpp').read_text()
dod_source = (root / 'viogpu/viogpudo/viogpudo.cpp').read_text()
dod_header = (root / 'viogpu/viogpudo/viogpudo.h').read_text()
method = 'SetNativeDiagnosticModeAndPath'
annotation = '__declspec(code_seg(".text"))\n'
outlined = ('AdmitNativeSubmitImports', 'DetachNativeHostPagingBatch')
noinline = '__declspec(noinline)\n'

def definition(name):
    text = dod_source if name == method else source
    qualified = 'VioGpuDod::' + name if name == method else name
    match = re.search(r'(?m)^(?:' + re.escape(annotation) + r')?(?:static )?(?:NTSTATUS|VOID|BOOLEAN) '
                      + qualified + r'\([^;{}]*\)\s*\{', text)
    if not match:
        raise ValueError('missing actual definition: ' + name)
    end, depth = match.end(), 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[match.start():end]

for name in REQUIRED:
    body = definition(name)
    if not body.startswith(annotation):
        raise SystemExit('missing resident definition: ' + name)
    if 'KeAcquireSpinLock' not in body:
        raise SystemExit('update scope: required owner no longer acquires a spinlock: ' + name)
    declarations = re.findall(r'(?m)^(?:' + re.escape(annotation) + r')?(?:static )?(?:NTSTATUS|VOID|BOOLEAN) '
                              + name + r'\([^;{}]*\);', source)
    if any(not declaration.startswith(annotation) for declaration in declarations):
        raise SystemExit('resident definition has unmatched forward declaration: ' + name)
if not re.search(r'(?m)^    ' + re.escape(annotation) + r'    NTSTATUS ' + method + r'\([^;{}]*\);', dod_header):
    raise SystemExit('resident diagnostic definition has unmatched class declaration')

name = 'ExecuteHostSurfacePaging'
declaration = re.search(r'(?m)^' + re.escape(annotation) + r'NTSTATUS ' + name + r'\([^;{}]*\);', source).group()
names = (name, 'RunNativeHostPagingWork')
bodies = {function: definition(function) for function in names}
failure = re.search(r'(?ms)^struct VIOGPU_HOST_SURFACE_PAGING_FAILURE\n\{.*?^};', source).group()
fixture = (here / 'residency_test.cpp').read_text().replace('// INSERT_FAILURE_RECORD', failure)

def sections(path):
    data = path.read_bytes()
    machine, count, _, symbols, symbol_count, optional, _ = struct.unpack_from('<HHLLLHH', data)
    assert machine == 0xaa64 and optional == 0
    section_names = {index + 1: data[20 + 40*index:28 + 40*index].split(b'\0', 1)[0].decode()
                     for index in range(count)}
    strings, result, index = symbols + symbol_count*18, {}, 0
    while index < symbol_count:
        offset = symbols + index*18
        raw = data[offset:offset + 8]
        if raw[:4] == b'\0'*4:
            begin = strings + struct.unpack_from('<L', raw, 4)[0]
            symbol = data[begin:data.index(b'\0', begin)].decode()
        else:
            symbol = raw.split(b'\0', 1)[0].decode()
        for function in names:
            if symbol.startswith('?' + function + '@'):
                result[function] = section_names.get(struct.unpack_from('<h', data, offset + 12)[0])
        index += 1 + data[offset + 17]
    return result

with tempfile.TemporaryDirectory(prefix='.native-ahb-residency-', dir=here) as temporary:
    output = Path(temporary)
    for removed in (None, *names):
        variant = removed or 'production'
        actual = '\n'.join(body.replace(annotation, '') if function == removed else body
                           for function, body in bodies.items())
        actual_declaration = declaration.replace(annotation, '') if removed == name else declaration
        unit, obj = output / (variant + '.cpp'), output / (variant + '.obj')
        unit.write_text(fixture.replace('// INSERT_DECLARATION', actual_declaration)
                       .replace('// INSERT_PRODUCTION', actual))
        subprocess.run(['clang++', '--target=aarch64-pc-windows-msvc', '-std=c++17', '-O0', '-fno-inline',
                        '-Wall', '-Wextra', '-Werror', '-c', str(unit), '-o', str(obj)], check=True)
        found = sections(obj)
        expected = {function: 'PAGE' if function == removed else '.text' for function in names}
        if found != expected:
            raise SystemExit(variant + ': wrong actual COFF placement: ' + repr(found))
        print('PASS ARM64 COFF ' + variant + (': resident bodies' if removed is None else ': PAGE regression detected'))
print(f'PASS all {len(REQUIRED)} production spinlock-owner definitions and declarations resident')
for function in outlined:
    # The WDK optimizer eliminated these stand-alone MAP entries once their
    # callers and bodies shared .text. Retain the final ten-symbol PE gate.
    definition_prefix = (r'(?m)^' + re.escape(noinline + annotation)
                         + r'(?:static )?(?:NTSTATUS|BOOLEAN) ' + function + r'\([^;{}]*\)\s*\{')
    if not re.search(definition_prefix, source):
        raise SystemExit('missing separately emitted resident owner: ' + function)
if not re.search(r'(?m)^' + re.escape(noinline + annotation)
                 + r'NTSTATUS AdmitNativeSubmitImports\([^;{}]*\);', source):
    raise SystemExit('outlined resident owner has unmatched forward declaration')
print('PASS optimized-MAP owners retain explicit out-of-line definitions')
