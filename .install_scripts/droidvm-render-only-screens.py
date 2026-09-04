#!/usr/bin/env python3
import copy
import json
import socket
import struct
import sys
import uuid
from pathlib import Path


VM_ID = "a5b6863f-6bbf-4af4-97f3-2a4653f6edb7"
RUN_ROOT = Path("/data/data/cn.classfun.droidvm/run")


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


def expected_screens(before: dict, mode: str) -> dict:
    screens = copy.deepcopy(before)
    gpu = screens.get("gpu-0")
    simplefb = screens.get("simplefb")
    if not isinstance(gpu, dict) or not isinstance(simplefb, dict):
        raise RuntimeError("both gpu-0 and simplefb screen records are required")
    if mode == "apply":
        expected_before = (True, "vnc", False, "none")
        expected_after = (True, "none", True, "vnc")
    else:
        expected_before = (True, "none", True, "vnc")
        expected_after = (True, "vnc", False, "none")
    actual_before = (
        gpu.get("enabled"),
        gpu.get("exporter"),
        simplefb.get("enabled"),
        simplefb.get("exporter"),
    )
    if actual_before != expected_before:
        raise RuntimeError(f"unexpected screen state for {mode}: {actual_before!r}")
    gpu["enabled"], gpu["exporter"], simplefb["enabled"], simplefb["exporter"] = expected_after
    return screens


def main() -> None:
    if len(sys.argv) != 2 or sys.argv[1] not in ("apply", "restore"):
        raise SystemExit("usage: droidvm-render-only-screens.py <apply|restore>")
    mode = sys.argv[1]
    token = (RUN_ROOT / "droidvmd-token.txt").read_text(encoding="utf-8").strip()
    port = int((RUN_ROOT / "droidvmd-port.txt").read_text(encoding="utf-8").strip())
    if len(token) != 32 or any(char not in "0123456789abcdefABCDEF" for char in token):
        raise RuntimeError("invalid DroidVM daemon token")
    if not 1 <= port <= 65535:
        raise RuntimeError(f"invalid daemon port: {port}")

    with socket.create_connection(("127.0.0.1", port), timeout=10) as daemon:
        request(daemon, "auth", token=token)
        status = request(daemon, "vm_status", vm_id=VM_ID)
        if status.get("state") != "stopped":
            raise RuntimeError(f"refusing to modify VM in state {status.get('state')!r}")
        before_response = request(daemon, "vm_get", vm_id=VM_ID)
        before = before_response.get("data")
        if not isinstance(before, dict):
            raise RuntimeError("vm_get returned no config")
        if before.get("id") != VM_ID or before.get("memory_mb") != 2048:
            raise RuntimeError("VM identity or 2048 MiB memory contract changed")
        if before.get("gpu_mode") != "native" or before.get("gpu_provider") != "drm2kgsl":
            raise RuntimeError("VM is not the Native Context drm2kgsl target")

        after = copy.deepcopy(before)
        after["screens"] = expected_screens(before.get("screens", {}), mode)
        changed = [key for key in before if before.get(key) != after.get(key)]
        if changed != ["screens"]:
            raise RuntimeError(f"unexpected top-level changes: {changed}")
        request(daemon, "vm_modify", config=after)
        verified = request(daemon, "vm_get", vm_id=VM_ID).get("data")
        if verified != after:
            raise RuntimeError("post-modify VM config does not match the exact requested state")

    print(
        json.dumps(
            {
                "mode": mode,
                "vm_id": VM_ID,
                "memory_mb": verified.get("memory_mb"),
                "before_screens": before.get("screens"),
                "after_screens": verified.get("screens"),
                "verified_narrow_change": True,
            },
            separators=(",", ":"),
        )
    )


if __name__ == "__main__":
    main()
