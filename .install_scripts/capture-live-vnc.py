#!/usr/bin/env python3
import hashlib
import json
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes


ANDROID_HOST = "192.168.60.237"
ANDROID_SSH_PORT = "8022"
LOCAL_VNC_HOST = "127.0.0.1"
LOCAL_VNC_PORT = 15900
REMOTE_HELPER = "/data/data/com.termux/files/home/read-live-vnc-config-20260830-noauth.py"
REMOTE_PYTHON = "/data/data/com.termux/files/usr/bin/python3"


def read_exact(connection: socket.socket, size: int) -> bytes:
    chunks = []
    remaining = size
    while remaining:
        chunk = connection.recv(remaining)
        if not chunk:
            raise RuntimeError("unexpected EOF from VNC server")
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def reverse_bits(value: int) -> int:
    result = 0
    for _ in range(8):
        result = (result << 1) | (value & 1)
        value >>= 1
    return result


def vnc_response(password: str, challenge: bytes) -> bytes:
    password_bytes = password.encode("latin-1")[:8].ljust(8, b"\0")
    key = bytes(reverse_bits(value) for value in password_bytes)
    encryptor = Cipher(algorithms.TripleDES(key), modes.ECB()).encryptor()
    return encryptor.update(challenge) + encryptor.finalize()


def request_update(connection: socket.socket, width: int, height: int) -> None:
    connection.sendall(struct.pack("!BBHHHH", 3, 0, 0, 0, width, height))


if len(sys.argv) != 2:
    raise SystemExit("usage: capture-live-vnc.py <output.ppm>")
output_path = Path(sys.argv[1])
if output_path.exists():
    raise SystemExit(f"refusing to overwrite capture: {output_path}")

endpoint_result = subprocess.run(
    [
        "ssh",
        "-o",
        "BatchMode=yes",
        "-p",
        ANDROID_SSH_PORT,
        ANDROID_HOST,
        f"su -c '{REMOTE_PYTHON} {REMOTE_HELPER}'",
    ],
    check=True,
    text=True,
    capture_output=True,
)
try:
    endpoint = json.loads(endpoint_result.stdout)
except json.JSONDecodeError as error:
    raise SystemExit("remote VNC endpoint helper returned invalid JSON") from error
if endpoint.get("host") != "127.0.0.1" or endpoint.get("port") != 5900:
    raise SystemExit("live VNC endpoint is not the expected Android loopback socket")

with socket.create_connection((LOCAL_VNC_HOST, LOCAL_VNC_PORT), timeout=10) as vnc:
    vnc.settimeout(20)
    banner = read_exact(vnc, 12)
    if not banner.startswith(b"RFB 003."):
        raise SystemExit(f"unexpected RFB banner: {banner!r}")
    vnc.sendall(b"RFB 003.008\n")

    security_count = read_exact(vnc, 1)[0]
    if security_count == 0:
        reason_size = struct.unpack("!I", read_exact(vnc, 4))[0]
        reason = read_exact(vnc, reason_size).decode("utf-8", "replace")
        raise SystemExit(f"VNC server rejected security negotiation: {reason}")
    security_types = read_exact(vnc, security_count)
    if 1 in security_types:
        vnc.sendall(b"\x01")
        security_name = "None"
    elif 2 in security_types:
        vnc.sendall(b"\x02")
        challenge = read_exact(vnc, 16)
        vnc.sendall(vnc_response(endpoint["password"], challenge))
        security_name = "VNCAuth"
    else:
        raise SystemExit(f"unsupported VNC security types: {list(security_types)}")
    security_result = struct.unpack("!I", read_exact(vnc, 4))[0]
    if security_result != 0:
        reason_size = struct.unpack("!I", read_exact(vnc, 4))[0]
        reason = read_exact(vnc, reason_size).decode("utf-8", "replace")
        raise SystemExit(f"VNC authentication failed: {reason}")

    vnc.sendall(b"\x01")
    server_init = read_exact(vnc, 24)
    width, height = struct.unpack("!HH", server_init[:4])
    name_size = struct.unpack("!I", server_init[20:24])[0]
    desktop_name = read_exact(vnc, name_size).decode("utf-8", "replace")
    if width <= 0 or height <= 0 or width > 8192 or height > 8192:
        raise SystemExit(f"invalid framebuffer size: {width}x{height}")

    pixel_format = struct.pack(
        "!BBBBHHHBBBxxx",
        32,
        24,
        0,
        1,
        255,
        255,
        255,
        16,
        8,
        0,
    )
    vnc.sendall(b"\x00\x00\x00\x00" + pixel_format)
    vnc.sendall(struct.pack("!BBHi", 2, 0, 1, 0))

    frame = bytearray(width * height * 4)
    raw_rectangles = 0
    deadline = time.monotonic() + 30
    request_update(vnc, width, height)
    while time.monotonic() < deadline and raw_rectangles == 0:
        try:
            message_type = read_exact(vnc, 1)[0]
        except TimeoutError:
            request_update(vnc, width, height)
            continue

        if message_type == 0:
            update_header = read_exact(vnc, 3)
            rectangle_count = struct.unpack("!H", update_header[1:3])[0]
            for _ in range(rectangle_count):
                x, y, rect_width, rect_height, encoding = struct.unpack(
                    "!HHHHi", read_exact(vnc, 12)
                )
                if encoding == 0:
                    if x + rect_width > width or y + rect_height > height:
                        raise SystemExit("raw VNC rectangle exceeds framebuffer")
                    pixels = read_exact(vnc, rect_width * rect_height * 4)
                    for row in range(rect_height):
                        source_start = row * rect_width * 4
                        target_start = ((y + row) * width + x) * 4
                        frame[target_start : target_start + rect_width * 4] = pixels[
                            source_start : source_start + rect_width * 4
                        ]
                    raw_rectangles += 1
                elif encoding == 1:
                    read_exact(vnc, 4)
                elif encoding == -224:
                    break
                else:
                    raise SystemExit(f"unsupported VNC encoding: {encoding}")
            if raw_rectangles == 0:
                request_update(vnc, width, height)
        elif message_type == 1:
            color_header = read_exact(vnc, 5)
            color_count = struct.unpack("!H", color_header[3:5])[0]
            read_exact(vnc, color_count * 6)
        elif message_type == 2:
            continue
        elif message_type == 3:
            cut_header = read_exact(vnc, 7)
            cut_size = struct.unpack("!I", cut_header[3:7])[0]
            read_exact(vnc, cut_size)
        else:
            raise SystemExit(f"unsupported VNC server message: {message_type}")

    if raw_rectangles == 0:
        raise SystemExit("timed out without a raw framebuffer update")

rgb = bytearray(width * height * 3)
rgb[0::3] = frame[2::4]
rgb[1::3] = frame[1::4]
rgb[2::3] = frame[0::4]
output_path.parent.mkdir(parents=True, exist_ok=True)
with output_path.open("xb") as output:
    output.write(f"P6\n{width} {height}\n255\n".encode("ascii"))
    output.write(rgb)

sample_step = max(1, width * height // 20000)
sample_colors = {
    bytes(rgb[index * 3 : index * 3 + 3])
    for index in range(0, width * height, sample_step)
}
print(
    json.dumps(
        {
            "pid": endpoint["pid"],
            "banner": banner.decode("ascii").strip(),
            "security": security_name,
            "width": width,
            "height": height,
            "desktop_name": desktop_name,
            "raw_rectangles": raw_rectangles,
            "rgb_sha256": hashlib.sha256(rgb).hexdigest(),
            "nonzero_rgb_bytes": sum(value != 0 for value in rgb),
            "sample_unique_colors": len(sample_colors),
            "output": str(output_path),
        },
        separators=(",", ":"),
    )
)
