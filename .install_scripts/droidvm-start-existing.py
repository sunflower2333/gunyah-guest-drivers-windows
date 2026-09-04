#!/usr/bin/env python3
import json
import socket
import struct
import sys
import uuid
from pathlib import Path


if len(sys.argv) != 2:
    raise SystemExit("usage: droidvm-start-existing.py <vm-id>")

vm_id = sys.argv[1]
run_root = Path("/data/data/cn.classfun.droidvm/run")
token = (run_root / "droidvmd-token.txt").read_text(encoding="utf-8").strip()
port = int((run_root / "droidvmd-port.txt").read_text(encoding="utf-8").strip())
if len(token) != 32 or any(char not in "0123456789abcdefABCDEF" for char in token):
    raise RuntimeError("invalid DroidVM daemon token")
if not 1 <= port <= 65535:
    raise RuntimeError(f"invalid DroidVM daemon port: {port}")


def read_exact(connection: socket.socket, size: int) -> bytes:
    chunks = []
    remaining = size
    while remaining:
        chunk = connection.recv(remaining)
        if not chunk:
            raise RuntimeError("unexpected EOF from DroidVM daemon")
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def receive_packet(connection: socket.socket) -> dict:
    size = struct.unpack("<I", read_exact(connection, 4))[0]
    if not 0 < size <= 8 * 1024 * 1024:
        raise RuntimeError(f"invalid daemon packet size: {size}")
    payload = read_exact(connection, size)
    if payload.endswith(b"\0"):
        payload = payload[:-1]
    return json.loads(payload.decode("utf-8"))


def request(connection: socket.socket, command: str, **fields) -> dict:
    request_id = str(uuid.uuid4())
    packet = {
        "type": "request",
        "request_id": request_id,
        "command": command,
        **fields,
    }
    payload = json.dumps(packet, separators=(",", ":")).encode("utf-8")
    connection.sendall(struct.pack("<I", len(payload)) + payload)
    while True:
        response = receive_packet(connection)
        if response.get("type") != "response" or response.get("request_id") != request_id:
            continue
        if not response.get("success", False):
            raise RuntimeError(f"{command}: {response.get('message', 'unknown daemon error')}")
        return response


with socket.create_connection(("127.0.0.1", port), timeout=10) as daemon:
    request(daemon, "auth", token=token)
    before = request(daemon, "vm_status", vm_id=vm_id)
    print(f"before={json.dumps(before, separators=(',', ':'))}")
    started = request(
        daemon,
        "vm_start",
        vm_id=vm_id,
        clear_logs_before_start=True,
    )
    print(f"start={json.dumps(started, separators=(',', ':'))}")
