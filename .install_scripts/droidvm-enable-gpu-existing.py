#!/usr/bin/env python3
import copy
import json
import socket
import struct
import sys
import uuid
from pathlib import Path


if len(sys.argv) != 2:
    raise SystemExit("usage: droidvm-enable-gpu-existing.py <vm-id>")

vm_id = sys.argv[1]
run_root = Path("/data/data/cn.classfun.droidvm/run")
token = (run_root / "droidvmd-token.txt").read_text(encoding="utf-8").strip()
port = int((run_root / "droidvmd-port.txt").read_text(encoding="utf-8").strip())
if len(token) != 32 or any(char not in "0123456789abcdefABCDEF" for char in token):
    raise RuntimeError("invalid DroidVM daemon token")
if not 1 <= port <= 65535:
    raise RuntimeError(f"invalid daemon port: {port}")


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
        raise RuntimeError(f"invalid response size: {size}")
    payload = read_exact(connection, size)
    if payload.endswith(b"\0"):
        payload = payload[:-1]
    return json.loads(payload.decode("utf-8"))


def request(connection: socket.socket, command: str, **fields) -> dict:
    request_id = str(uuid.uuid4())
    payload = json.dumps(
        {"type": "request", "request_id": request_id, "command": command, **fields},
        separators=(",", ":"),
    ).encode("utf-8")
    connection.sendall(struct.pack("<I", len(payload)) + payload)
    while True:
        response = receive_packet(connection)
        if response.get("type") != "response" or response.get("request_id") != request_id:
            continue
        if not response.get("success", False):
            raise RuntimeError(f"{command}: {response.get('message', 'request failed')}")
        return response


with socket.create_connection(("127.0.0.1", port), timeout=10) as daemon:
    request(daemon, "auth", token=token)
    status = request(daemon, "vm_status", vm_id=vm_id)
    state = status.get("state")
    if state != "stopped":
        raise RuntimeError(f"refusing to modify VM in state {state!r}")
    before_response = request(daemon, "vm_get", vm_id=vm_id)
    before = before_response.get("data")
    if not isinstance(before, dict):
        raise RuntimeError("vm_get returned no config")
    if before.get("gpu_mode") != "native" or before.get("gpu_provider") != "drm2kgsl":
        raise RuntimeError("refusing to modify a non-native drm2kgsl VM")
    after = copy.deepcopy(before)
    after.setdefault("screens", {}).setdefault("gpu-0", {})["enabled"] = True
    changed = [key for key in before if before.get(key) != after.get(key)]
    if changed != ["screens"]:
        raise RuntimeError(f"unexpected top-level changes: {changed}")
    before_screens = before.get("screens", {})
    after_screens = after.get("screens", {})
    for screen_name, screen in before_screens.items():
        if screen_name == "gpu-0":
            expected = copy.deepcopy(screen)
            expected["enabled"] = True
            if after_screens.get(screen_name) != expected:
                raise RuntimeError("gpu-0 change was not narrow")
        elif after_screens.get(screen_name) != screen:
            raise RuntimeError(f"unexpected change to screen {screen_name}")
    modified = request(daemon, "vm_modify", config=after)
    verified_response = request(daemon, "vm_get", vm_id=vm_id)
    verified = verified_response.get("data")
    if not isinstance(verified, dict):
        raise RuntimeError("post-modify vm_get returned no config")
    if verified.get("screens", {}).get("gpu-0", {}).get("enabled") is not True:
        raise RuntimeError("gpu-0 did not become enabled")
    check = copy.deepcopy(verified)
    check.setdefault("screens", {}).setdefault("gpu-0", {})["enabled"] = False
    if check != before:
        raise RuntimeError("post-modify config differs beyond gpu-0.enabled")
    print(json.dumps({
        "state_before": state,
        "before_gpu_screen": before.get("screens", {}).get("gpu-0"),
        "modified": modified,
        "after_gpu_screen": verified.get("screens", {}).get("gpu-0"),
        "verified_narrow_change": True,
    }, separators=(",", ":")))
