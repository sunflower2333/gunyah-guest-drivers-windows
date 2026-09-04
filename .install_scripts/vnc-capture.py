#!/usr/bin/env python3
"""Capture the crosvm VNC framebuffer to a PNG.

Runs on the Android host so the raw framebuffer never crosses the slow tunnel;
only the compressed PNG is relayed back.
"""
import socket
import struct
import sys
import zlib


def read_exact(s, n):
    buf = b''
    while len(buf) < n:
        c = s.recv(n - len(buf))
        if not c:
            raise RuntimeError('EOF from VNC server')
        buf += c
    return buf


def write_png(path, width, height, rgb_rows):
    raw = b''.join(b'\x00' + row for row in rgb_rows)

    def chunk(tag, data):
        c = struct.pack('>I', len(data)) + tag + data
        return c + struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff)

    png = b'\x89PNG\r\n\x1a\n'
    png += chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(raw, 6))
    png += chunk(b'IEND', b'')
    open(path, 'wb').write(png)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else 'vnc.png'
    host = sys.argv[2] if len(sys.argv) > 2 else '127.0.0.1'
    port = int(sys.argv[3]) if len(sys.argv) > 3 else 5900

    s = socket.create_connection((host, port), timeout=30)
    s.settimeout(60)

    server_version = read_exact(s, 12)
    s.sendall(b'RFB 003.008\n')

    n = read_exact(s, 1)[0]
    if n == 0:
        reason_len = struct.unpack('>I', read_exact(s, 4))[0]
        raise RuntimeError('VNC refused: ' + read_exact(s, reason_len).decode('latin-1'))
    types = read_exact(s, n)
    if 1 not in types:
        raise RuntimeError('server requires auth types %s; only None is supported here' % list(types))
    s.sendall(bytes([1]))
    if struct.unpack('>I', read_exact(s, 4))[0] != 0:
        raise RuntimeError('VNC security handshake failed')

    s.sendall(bytes([1]))  # ClientInit, shared
    width, height = struct.unpack('>HH', read_exact(s, 4))
    pf = read_exact(s, 16)
    name_len = struct.unpack('>I', read_exact(s, 4))[0]
    name = read_exact(s, name_len).decode('latin-1')

    bpp, depth, big_endian, true_colour = pf[0], pf[1], pf[2], pf[3]
    rmax, gmax, bmax = struct.unpack('>HHH', pf[4:10])
    rshift, gshift, bshift = pf[10], pf[11], pf[12]
    print('server=%s name=%s %dx%d bpp=%d depth=%d be=%d tc=%d shifts=%d/%d/%d'
          % (server_version.decode().strip(), name, width, height, bpp, depth,
             big_endian, true_colour, rshift, gshift, bshift))
    if bpp != 32 or not true_colour:
        raise RuntimeError('only 32bpp true-colour is handled, got bpp=%d tc=%d' % (bpp, true_colour))

    s.sendall(struct.pack('>BBHHHH', 3, 0, 0, 0, width, height))  # full update

    rows = [bytearray(width * 3) for _ in range(height)]
    got = 0
    while got < 1:
        msg = read_exact(s, 1)[0]
        if msg != 0:
            raise RuntimeError('unexpected VNC message type %d' % msg)
        read_exact(s, 1)
        nrect = struct.unpack('>H', read_exact(s, 2))[0]
        for _ in range(nrect):
            x, y, w, h = struct.unpack('>HHHH', read_exact(s, 8))
            enc = struct.unpack('>i', read_exact(s, 4))[0]
            if enc != 0:
                raise RuntimeError('only Raw encoding is handled, got %d' % enc)
            data = read_exact(s, w * h * 4)
            fmt = '>I' if big_endian else '<I'
            for row in range(h):
                base = row * w * 4
                target = rows[y + row]
                for col in range(w):
                    px = struct.unpack_from(fmt, data, base + col * 4)[0]
                    o = (x + col) * 3
                    target[o] = (px >> rshift) & rmax
                    target[o + 1] = (px >> gshift) & gmax
                    target[o + 2] = (px >> bshift) & bmax
            got += 1
    s.close()

    write_png(out, width, height, [bytes(r) for r in rows])
    print('wrote %s (%dx%d)' % (out, width, height))


if __name__ == '__main__':
    main()
