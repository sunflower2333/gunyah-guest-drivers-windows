#!/usr/bin/env python3
"""Resolve an image-relative address to the enclosing public symbol in a MSVC .map.

The KMD records provenance as an RVA (return address minus __ImageBase).  The
linker map lists publics as Rva+Base against a preferred load address, so the
symbol RVA is Rva+Base - preferred_base.  Report the nearest public at or below
the target, with its offset, plus the following symbol as an upper bound.
"""
import re
import sys


def load(map_path):
    text = open(map_path, "r", errors="replace").read()
    m = re.search(r"Preferred load address is\s+([0-9A-Fa-f]+)", text)
    if not m:
        raise SystemExit("no preferred load address in map")
    base = int(m.group(1), 16)
    syms = []
    for line in text.splitlines():
        # " 0001:00001f60       ?Foo@@YAXXZ    0000000140002f60     lib:obj"
        mm = re.match(r"\s+[0-9A-Fa-f]{4}:[0-9A-Fa-f]{8}\s+(\S+)\s+([0-9A-Fa-f]{8,16})\s", line)
        if mm:
            syms.append((int(mm.group(2), 16) - base, mm.group(1)))
    syms.sort()
    return base, syms


def resolve(syms, rva):
    lo, hi = 0, len(syms) - 1
    best = None
    while lo <= hi:
        mid = (lo + hi) // 2
        if syms[mid][0] <= rva:
            best = mid
            lo = mid + 1
        else:
            hi = mid - 1
    return best


def main():
    if len(sys.argv) < 3:
        raise SystemExit("usage: resolve-map-rva.py <map> <rva-hex-or-dec> [...]")
    base, syms = load(sys.argv[1])
    print(f"map={sys.argv[1]} preferred_base=0x{base:x} publics={len(syms)}")
    for arg in sys.argv[2:]:
        rva = int(arg, 0)
        i = resolve(syms, rva)
        if i is None:
            print(f"RVA 0x{rva:x}: below the first public")
            continue
        sym_rva, name = syms[i]
        nxt = f"  next=0x{syms[i+1][0]:x} {syms[i+1][1]}" if i + 1 < len(syms) else ""
        print(f"RVA 0x{rva:x} -> {name}  (symbol 0x{sym_rva:x}, +0x{rva - sym_rva:x}){nxt}")


if __name__ == "__main__":
    main()
