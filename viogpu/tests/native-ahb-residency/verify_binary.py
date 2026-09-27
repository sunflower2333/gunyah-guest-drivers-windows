#!/usr/bin/env python3
"""Verify linked native AHB spinlock owners are in executable, nonpageable .text."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct

REQUIRED = (
    'RunNativeHostPagingWork', 'DetachNativeHostPagingBatch', 'PinNativeSubmitImports',
    'AdmitNativeSubmitImports', 'ExecuteHostSurfacePaging', 'EnsureNativeHostSurfaceBinding',
    'PresentHostSurface', 'ExportNativeShareLocked', 'HandleNativePublishEscape',
    'SetNativeDiagnosticModeAndPath',
)

def verify(binary, mapfile):
    data = binary.read_bytes()
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    if data[pe:pe + 4] != b'PE\0\0':
        raise ValueError('not a PE image')
    machine, count, timestamp, _, _, optional, _ = struct.unpack_from('<HHIIIHH', data, pe + 4)
    opt = pe + 24
    if machine != 0xaa64 or struct.unpack_from('<H', data, opt)[0] != 0x20b:
        raise ValueError('expected ARM64 PE32+')
    base = struct.unpack_from('<Q', data, opt + 24)[0]
    maptext = mapfile.read_text()
    map_timestamp = re.search(r'Timestamp is ([0-9a-f]+)', maptext, re.I)
    map_base = re.search(r'Preferred load address is ([0-9a-f]+)', maptext, re.I)
    if not map_timestamp or int(map_timestamp[1], 16) != timestamp:
        raise ValueError('MAP timestamp does not match PE image')
    if not map_base or int(map_base[1], 16) != base:
        raise ValueError('MAP preferred base does not match PE image')
    sections = []
    for index in range(count):
        offset = opt + optional + index * 40
        name = data[offset:offset + 8].split(b'\0', 1)[0].decode('ascii')
        virtual_size, rva, raw_size = struct.unpack_from('<III', data, offset + 8)
        flags = struct.unpack_from('<I', data, offset + 36)[0]
        sections.append((name, rva, max(virtual_size, raw_size), flags))
    found = {}
    for match in re.finditer(r'^\s+[0-9a-f]{4}:[0-9a-f]+\s+\?([^@]+)@\S+\s+([0-9a-f]{16})\s+f\s+\S+',
                             maptext, re.M | re.I):
        name = match[1]
        if name not in REQUIRED:
            continue
        if name in found:
            raise ValueError('duplicate required function: ' + name)
        rva = int(match[2], 16) - base
        candidates = [section for section in sections if section[1] <= rva < section[1] + section[2]]
        if len(candidates) != 1:
            raise ValueError(f'{name}: RVA is not in exactly one PE section')
        section = candidates[0]
        found[name] = {'rva': hex(rva), 'section': section[0], 'characteristics': hex(section[3]),
                       'resident': section[0] == '.text' and bool(section[3] & 0x20000000)
                                   and bool(section[3] & 0x08000000)
                                   and not bool(section[3] & 0x02000000)}
    if set(found) != set(REQUIRED):
        raise ValueError('required MAP functions missing: ' + repr(sorted(set(REQUIRED) - set(found))))
    return {'binary_sha256': hashlib.sha256(data).hexdigest(),
            'map_sha256': hashlib.sha256(mapfile.read_bytes()).hexdigest(),
            'timestamp': hex(timestamp), 'functions': found,
            'passed': all(value['resident'] for value in found.values())}

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('binary', type=Path)
    parser.add_argument('mapfile', type=Path)
    parser.add_argument('--expect-page-regression', action='store_true',
                        help='Negative control: require all original functions in PAGE')
    args = parser.parse_args()
    result = verify(args.binary, args.mapfile)
    print(json.dumps(result, indent=2))
    if args.expect_page_regression:
        if result['passed'] or not all(value['section'] == 'PAGE' for value in result['functions'].values()):
            raise SystemExit('expected original PAGE regression for all required functions')
        print(f'PASS original binary rejected: all {len(REQUIRED)} spinlock owners are in PAGE')
    elif not result['passed']:
        raise SystemExit('FAIL native AHB spinlock owner remains pageable')

if __name__ == '__main__':
    main()
