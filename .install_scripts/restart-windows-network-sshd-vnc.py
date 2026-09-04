#!/usr/bin/env python3
import json
import socket
import struct
import subprocess
import time

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes


ANDROID_HOST = "192.168.60.237"
ANDROID_SSH_PORT = "8022"
LOCAL_VNC_HOST = "127.0.0.1"
LOCAL_VNC_PORT = 15900
REMOTE_HELPER = "/data/data/com.termux/files/home/read-live-vnc-config-20260830-noauth.py"
REMOTE_PYTHON = "/data/data/com.termux/files/usr/bin/python3"
RUN_COMMAND = (
    'powershell.exe -NoProfile -Command "Get-NetAdapter | Where-Object Status '
    '-ne Disabled | Restart-NetAdapter -Confirm:$false; Restart-Service -Name '
    'sshd -Force"'
)

XK_CONTROL_L = 0xFFE3
XK_SHIFT_L = 0xFFE1
XK_SUPER_L = 0xFFEB
XK_LEFT = 0xFF51
XK_RETURN = 0xFF0D


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


def send_key(connection: socket.socket, keysym: int, down: bool) -> None:
    connection.sendall(struct.pack("!BBHI", 4, int(down), 0, keysym))


def tap_key(connection: socket.socket, keysym: int) -> None:
    send_key(connection, keysym, True)
    send_key(connection, keysym, False)


def send_chord(connection: socket.socket, modifiers: list[int], keysym: int) -> None:
    for modifier in modifiers:
        send_key(connection, modifier, True)
    tap_key(connection, keysym)
    for modifier in reversed(modifiers):
        send_key(connection, modifier, False)


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
    vnc.settimeout(10)
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

    send_chord(vnc, [XK_SUPER_L], ord("r"))
    time.sleep(1.5)
    for character in RUN_COMMAND:
        tap_key(vnc, ord(character))
        time.sleep(0.01)
    send_chord(vnc, [XK_CONTROL_L, XK_SHIFT_L], XK_RETURN)
    time.sleep(2)

    # UAC defaults to No. Left selects Yes; on systems without a prompt this
    # only sends an empty cursor/Enter sequence to the launched shell.
    tap_key(vnc, XK_LEFT)
    tap_key(vnc, XK_RETURN)
    time.sleep(4)

print(
    json.dumps(
        {
            "pid": endpoint["pid"],
            "banner": banner.decode("ascii").strip(),
            "width": width,
            "height": height,
            "desktop_name": desktop_name,
            "security": security_name,
            "action": "restart-network-and-sshd",
        },
        separators=(",", ":"),
    )
)
