#!/usr/bin/env python3
import hashlib
import json
import os
import subprocess
from pathlib import Path


VM_ID = "a5b6863f-6bbf-4af4-97f3-2a4653f6edb7"
CONFIG = Path("/data/data/cn.classfun.droidvm/files/vms.json")


def process_ids(name: str) -> list[str]:
    result = subprocess.run(
        ["/system/bin/pidof", name],
        check=False,
        text=True,
        capture_output=True,
    )
    if result.returncode not in (0, 1):
        raise RuntimeError(f"pidof {name} failed with {result.returncode}")
    return result.stdout.split()


def redact(path: str, value: object) -> object:
    if "password" in path.lower():
        if isinstance(value, bool):
            return value
        return {"type": type(value).__name__, "set": bool(value)}
    return value


def collect_vnc_fields(value: object, path: str = "") -> dict[str, object]:
    fields = {}
    if isinstance(value, dict):
        for key, child in value.items():
            child_path = f"{path}.{key}" if path else key
            if "vnc" in child_path.lower() or "password" in key.lower():
                if isinstance(child, (dict, list)):
                    fields.update(collect_vnc_fields(child, child_path))
                else:
                    fields[child_path] = redact(child_path, child)
    elif isinstance(value, list):
        for index, child in enumerate(value):
            fields.update(collect_vnc_fields(child, f"{path}[{index}]"))
    return fields


if os.getuid() != 0:
    raise SystemExit("must run as root")
if not CONFIG.is_file():
    raise SystemExit(f"missing config: {CONFIG}")

raw = CONFIG.read_bytes()
data = json.loads(raw.decode("utf-8"))
vms = data.get("vms")
if not isinstance(vms, list):
    raise RuntimeError("top-level vms field is not an array")
matches = [vm for vm in vms if isinstance(vm, dict) and vm.get("id") == VM_ID]
if len(matches) != 1:
    raise RuntimeError(f"expected one VM {VM_ID}, found {len(matches)}")
vm = matches[0]

stat = subprocess.run(
    ["/system/bin/stat", "-c", "%u:%g:%a:%s:%C", str(CONFIG)],
    check=True,
    text=True,
    capture_output=True,
).stdout.strip()

print(
    json.dumps(
        {
            "config_sha256": hashlib.sha256(raw).hexdigest(),
            "config_metadata": stat,
            "vm_count": len(vms),
            "vm_id": vm.get("id"),
            "vm_name": vm.get("name"),
            "memory_mb": vm.get("memory_mb"),
            "crosvm_pids": process_ids("crosvm"),
            "droidvmd_pids": process_ids("droidvmd"),
            "vnc_fields": collect_vnc_fields(vm),
        },
        separators=(",", ":"),
        sort_keys=True,
    )
)
