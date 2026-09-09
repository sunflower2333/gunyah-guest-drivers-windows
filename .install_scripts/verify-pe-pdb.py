#!/usr/bin/env python3
"""Require a PE's CodeView GUID/age to match the supplied MSF 7 PDB."""

import argparse
import struct
import uuid
from pathlib import Path


def u32(data, offset):
    return struct.unpack_from('<I', data, offset)[0]


def pe_identity(data):
    pe = u32(data, 0x3c)
    if data[:2] != b'MZ' or data[pe:pe + 4] != b'PE\0\0':
        raise ValueError('not a PE image')
    sections, = struct.unpack_from('<H', data, pe + 6)
    opt_size, = struct.unpack_from('<H', data, pe + 20)
    opt = pe + 24
    magic, = struct.unpack_from('<H', data, opt)
    if magic not in (0x10b, 0x20b):
        raise ValueError('unknown PE optional header')
    dirs = opt + (112 if magic == 0x20b else 96)

    def rva_offset(rva):
        for i in range(sections):
            section = opt + opt_size + 40 * i
            size, start, raw_size, raw = struct.unpack_from('<IIII', data, section + 8)
            if start <= rva < start + max(size, raw_size):
                return raw + rva - start
        raise ValueError('debug directory RVA has no section')

    debug_rva, debug_size = struct.unpack_from('<II', data, dirs + 6 * 8)
    debug = rva_offset(debug_rva)
    for offset in range(debug, debug + debug_size, 28):
        if u32(data, offset + 12) != 2:  # IMAGE_DEBUG_TYPE_CODEVIEW
            continue
        raw = u32(data, offset + 24)
        if data[raw:raw + 4] == b'RSDS':
            return uuid.UUID(bytes_le=data[raw + 4:raw + 20]), u32(data, raw + 20)
    raise ValueError('PE has no RSDS CodeView identity')


def pdb_identity(data):
    if data[:32] != b'Microsoft C/C++ MSF 7.00\r\n\x1aDS\0\0\0':
        raise ValueError('not an MSF 7 PDB')
    block_size, _, block_count, directory_size, _, block_map = struct.unpack_from('<6I', data, 32)
    if block_size < 512 or block_size > 65536 or block_size & (block_size - 1):
        raise ValueError('invalid PDB block size')
    if block_count * block_size > len(data):
        raise ValueError('truncated PDB')

    def blocks(offset, count):
        result = bytearray()
        for i in range(count):
            block = u32(data, offset + i * 4)
            if block >= block_count:
                raise ValueError('invalid PDB block index')
            result.extend(data[block * block_size:(block + 1) * block_size])
        return result

    directory = blocks(block_map * block_size, (directory_size + block_size - 1) // block_size)
    count = u32(directory, 0)
    if count < 2 or 4 + count * 4 > directory_size:
        raise ValueError('invalid PDB stream directory')
    cursor = 4 + count * 4
    info = None
    for i in range(count):
        size = u32(directory, 4 + i * 4)
        pages = 0 if size == 0xffffffff else (size + block_size - 1) // block_size
        if cursor + pages * 4 > directory_size:
            raise ValueError('truncated PDB stream directory')
        if i == 1:
            if size < 28 or size == 0xffffffff or pages == 0:
                raise ValueError('PDB info stream missing')
            block = u32(directory, cursor)
            if block >= block_count:
                raise ValueError('invalid PDB info block')
            info = data[block * block_size:block * block_size + 28]
            break
        cursor += pages * 4
    return uuid.UUID(bytes_le=info[12:28]), u32(info, 8)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('pe', type=Path)
    parser.add_argument('pdb', type=Path)
    args = parser.parse_args()
    try:
        image = pe_identity(args.pe.read_bytes())
        symbols = pdb_identity(args.pdb.read_bytes())
        if image != symbols:
            raise ValueError(f'PE {image} != PDB {symbols}')
    except (OSError, ValueError, struct.error) as error:
        parser.exit(1, f'PE/PDB mismatch: {error}\n')
    print(f'PE/PDB match: GUID={image[0]} age={image[1]}')


if __name__ == '__main__':
    main()
