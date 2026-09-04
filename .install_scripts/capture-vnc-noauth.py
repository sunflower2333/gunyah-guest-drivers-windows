#!/usr/bin/env python3
"""Capture one framebuffer from the crosvm VNC server (RFB 3.8, security type None).

The server fronts SimpleFB, which is the Windows desktop, so this shows exactly
what a user would see on the guest.
"""
import argparse
import socket
import struct
import sys


def read_exact(sock, size):
    buf = bytearray()
    while len(buf) < size:
        chunk = sock.recv(min(1 << 16, size - len(buf)))
        if not chunk:
            raise RuntimeError("RFB connection closed early")
        buf.extend(chunk)
    return bytes(buf)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("output")
    ap.add_argument("--host", default="192.168.60.237")
    ap.add_argument("--port", type=int, default=5900)
    ap.add_argument("--timeout", type=float, default=60.0)
    args = ap.parse_args()

    s = socket.create_connection((args.host, args.port), timeout=args.timeout)
    s.settimeout(args.timeout)

    server_version = read_exact(s, 12)
    if not server_version.startswith(b"RFB "):
        raise SystemExit(f"not an RFB server: {server_version!r}")
    s.sendall(b"RFB 003.008\n")

    count = read_exact(s, 1)[0]
    if count == 0:
        reason_len = struct.unpack("!I", read_exact(s, 4))[0]
        raise SystemExit("server refused: " + read_exact(s, reason_len).decode(errors="replace"))
    types = read_exact(s, count)
    if 1 not in types:
        raise SystemExit(f"server requires auth, offered types {list(types)}")
    s.sendall(bytes([1]))
    result = struct.unpack("!I", read_exact(s, 4))[0]
    if result != 0:
        raise SystemExit(f"security handshake failed: {result}")

    s.sendall(bytes([1]))  # shared
    width, height = struct.unpack("!HH", read_exact(s, 4))
    pixel_format = read_exact(s, 16)
    name_len = struct.unpack("!I", read_exact(s, 4))[0]
    read_exact(s, name_len)
    bpp, depth, big_endian, true_colour = pixel_format[0], pixel_format[1], pixel_format[2], pixel_format[3]
    if bpp != 32:
        raise SystemExit(f"unexpected bpp {bpp}")

    s.sendall(struct.pack("!BBHI", 2, 0, 1, 0))          # SetEncodings: raw
    s.sendall(struct.pack("!BBHHHH", 3, 0, 0, 0, width, height))  # FramebufferUpdateRequest

    while True:
        msg = read_exact(s, 1)[0]
        if msg != 0:
            continue
        read_exact(s, 1)
        rects = struct.unpack("!H", read_exact(s, 2))[0]
        pixels = bytearray(width * height * 4)
        for _ in range(rects):
            x, y, w, h, enc = struct.unpack("!HHHHi", read_exact(s, 12))
            if enc != 0:
                raise SystemExit(f"unexpected encoding {enc}")
            data = read_exact(s, w * h * 4)
            for row in range(h):
                dst = ((y + row) * width + x) * 4
                src = row * w * 4
                pixels[dst:dst + w * 4] = data[src:src + w * 4]
        break

    # RFB raw 32bpp little-endian true colour with shifts r16 g8 b0 -> BGRX order.
    with open(args.output, "wb") as fh:
        fh.write(b"P6\n%d %d\n255\n" % (width, height))
        for i in range(0, len(pixels), 4):
            fh.write(bytes((pixels[i + 2], pixels[i + 1], pixels[i])))
    print(f"WROTE {args.output} {width}x{height}")


if __name__ == "__main__":
    main()
