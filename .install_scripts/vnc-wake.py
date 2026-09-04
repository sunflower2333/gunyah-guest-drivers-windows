#!/usr/bin/env python3
"""Send a harmless key press over VNC to wake a blanked guest display."""
import socket, struct, sys, time

def read_exact(s, n):
    b = b''
    while len(b) < n:
        c = s.recv(n - len(b))
        if not c:
            raise RuntimeError('EOF')
        b += c
    return b

host = sys.argv[1] if len(sys.argv) > 1 else '127.0.0.1'
port = int(sys.argv[2]) if len(sys.argv) > 2 else 5900

s = socket.create_connection((host, port), timeout=30)
s.settimeout(30)
read_exact(s, 12)
s.sendall(b'RFB 003.008\n')
n = read_exact(s, 1)[0]
types = read_exact(s, n)
if 1 not in types:
    raise SystemExit('no None auth; types=%s' % list(types))
s.sendall(bytes([1]))
if struct.unpack('>I', read_exact(s, 4))[0] != 0:
    raise SystemExit('security failed')
s.sendall(bytes([1]))
w, h = struct.unpack('>HH', read_exact(s, 4))
read_exact(s, 16)
nl = struct.unpack('>I', read_exact(s, 4))[0]
read_exact(s, nl)
print('connected %dx%d' % (w, h))

# Shift is inert: it wakes the session without typing or activating anything.
XK_SHIFT_L = 0xffe1
for down in (1, 0):
    s.sendall(struct.pack('>BBHI', 4, down, 0, XK_SHIFT_L))
    time.sleep(0.15)

# A small pointer move is what actually clears most blank/idle states.
for (x, y) in ((w // 2, h // 2), (w // 2 + 24, h // 2 + 18), (w // 2, h // 2)):
    s.sendall(struct.pack('>BBHH', 5, 0, x, y))
    time.sleep(0.15)

time.sleep(1.5)
s.close()
print('wake events sent')
