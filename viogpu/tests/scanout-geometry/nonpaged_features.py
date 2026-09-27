#!/usr/bin/env python3
"""Compile actual DISPATCH feature predicates into ARM64 Windows COFF sections."""
from pathlib import Path
import re
import struct
import subprocess
import tempfile

here = Path(__file__).resolve().parent
root = here.parents[2]
source = (root / 'viogpu/viogpudo/viogpudo.cpp').read_text()
header = (root / 'viogpu/viogpudo/viogpudo.h').read_text()
adapter_header = header[header.index('class VioGpuAdapter :'):header.index('class VioGpuDod\n')]
wire = (root / 'viogpu/common/viogpu_3d_wire.h').read_text()
names = ('SupportsNativeAhbPaging', 'SupportsNativeScanoutGeometry')
bodies = {}
declarations = {}
for name in names:
    match = re.search(r'(?m)^(?:__declspec\(code_seg\("[^"]+"\)\)\n)?'
                      r'BOOLEAN VioGpuAdapter::' + name + r'\(\) const\n\{[^}]+\}', source)
    if not match:
        raise RuntimeError('missing feature definition: ' + name)
    bodies[name] = match.group()
    match = re.search(r'(?m)^\s*(?:__declspec\(code_seg\("[^"]+"\)\) )?'
                      r'BOOLEAN ' + name + r'\(\) const;', adapter_header)
    if not match:
        raise RuntimeError('missing actual predicate declaration: ' + name)
    declarations[name] = match.group().strip()
defines = []
for name in ('NATIVE_AHB_V2', 'NATIVE_AHB_RELEASE', 'NATIVE_AHB_PAGING', 'NATIVE_SCANOUT_GEOMETRY'):
    match = re.search(r'(?m)^#define VIRTIO_GPU_F_' + name + r'\s+\d+\s*$', wire)
    if not match:
        raise RuntimeError('missing feature constant: ' + name)
    defines.append(match.group())
fixture = '\n'.join(defines) + '''
using BOOLEAN = unsigned char;
inline bool virtio_is_feature_enabled(unsigned long long bits, unsigned bit) {
    return (bits & (1ULL << bit)) != 0;
}
class VioGpuAdapter {
public:
    unsigned long long m_u64GuestFeatures;
    // INSERT_DECLARATIONS
};
#pragma code_seg("PAGE")
'''

def coff_sections(path):
    data = path.read_bytes()
    machine, count, _, symbols, symbol_count, optional, _ = struct.unpack_from('<HHLLLHH', data)
    if machine != 0xaa64 or optional:
        raise RuntimeError('expected ARM64 COFF object')
    sections = {}
    for index in range(count):
        offset = 20 + optional + 40 * index
        sections[index + 1] = data[offset:offset + 8].split(b'\0', 1)[0].decode()
    strings = symbols + symbol_count * 18
    result = {}
    index = 0
    while index < symbol_count:
        offset = symbols + index * 18
        raw = data[offset:offset + 8]
        if raw[:4] == b'\0' * 4:
            start = strings + struct.unpack_from('<L', raw, 4)[0]
            name = data[start:data.index(b'\0', start)].decode()
        else:
            name = raw.split(b'\0', 1)[0].decode()
        section = struct.unpack_from('<h', data, offset + 12)[0]
        for helper in names:
            if name.startswith('?' + helper + '@VioGpuAdapter@@'):
                if helper in result:
                    raise RuntimeError('duplicate code symbol: ' + helper)
                result[helper] = sections.get(section)
        index += 1 + data[offset + 17]
    if set(result) != set(names):
        raise RuntimeError('missing compiled predicates: ' + repr(result))
    return result

with tempfile.TemporaryDirectory(prefix='.nonpaged-features-', dir=here) as temporary:
    output = Path(temporary)
    for removed in (None, *names):
        name = removed or 'production'
        unit, obj = output / (name + '.cpp'), output / (name + '.obj')
        actual = '\n'.join(body.replace('__declspec(code_seg(".text"))\n', '')
                           if helper == removed else body for helper, body in bodies.items())
        actual_declarations = '\n'.join(declaration.replace('__declspec(code_seg(".text")) ', '')
                                       if helper == removed else declaration
                                       for helper, declaration in declarations.items())
        unit.write_text(fixture.replace('// INSERT_DECLARATIONS', actual_declarations) + actual + '\n')
        subprocess.run(['clang++', '--target=aarch64-pc-windows-msvc', '-std=c++17', '-O0', '-fno-inline',
                        '-Wall', '-Wextra', '-Werror', '-c', str(unit), '-o', str(obj)], check=True)
        found = coff_sections(obj)
        expected = {helper: 'PAGE' if helper == removed else '.text' for helper in names}
        if found != expected:
            raise SystemExit(name + ': unexpected ARM64 COFF placement: ' + repr(found))
        print('PASS ARM64 COFF ' + name + (': .text predicates' if removed is None else ': PAGE regression detected'))
