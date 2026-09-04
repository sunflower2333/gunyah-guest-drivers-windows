#!/usr/bin/env python3
import copy
import hashlib
import json
import os
import subprocess
import time
from pathlib import Path


VM_ID = "a5b6863f-6bbf-4af4-97f3-2a4653f6edb7"
CONFIG = Path("/data/data/cn.classfun.droidvm/files/vms.json")
BACKUP = Path(
    "/data/data/cn.classfun.droidvm/files/"
    "vms.json.pre-shim-parcel-20260830-005124"
)
TEMP = Path(
    "/data/data/cn.classfun.droidvm/files/"
    "vms.json.new-shim-parcel-20260830-005124"
)
EXPECTED_PREIMAGE_HASH = (
    "ceed79b596a6a324eeca266311e50b628e576c86b6bb1d3395ef1aa1d474054a"
)
ACTIVE_DISK = "/mnt/pass_through/0/emulated/0/win11-droidvm-final-comp.qcow2"
ENVIRONMENT_ENTRY = "DROIDVM_SHIM_PARCEL_MB=256"
APP_PACKAGE = "cn.classfun.droidvm"
APP_COMPONENT = "cn.classfun.droidvm/.ui.SplashActivity"


def run(command: list[str], *, capture: bool = False) -> subprocess.CompletedProcess:
    return subprocess.run(
        command,
        check=True,
        text=True,
        capture_output=capture,
    )


def sha256(raw: bytes) -> str:
    return hashlib.sha256(raw).hexdigest()


def serialize(data: object) -> bytes:
    text = json.dumps(data, ensure_ascii=True, indent=4)
    return text.replace("/", "\\/").encode("utf-8")


def require_equal(actual: object, expected: object, name: str) -> None:
    if actual != expected:
        raise RuntimeError(f"unexpected {name}: {actual!r}")


def validate_config(data: object) -> dict:
    if not isinstance(data, dict):
        raise RuntimeError("top-level config is not an object")
    vms = data.get("vms")
    if not isinstance(vms, list) or len(vms) != 1:
        raise RuntimeError("config must contain exactly one VM")
    vm = vms[0]
    if not isinstance(vm, dict):
        raise RuntimeError("VM entry is not an object")

    require_equal(vm.get("id"), VM_ID, "VM UUID")
    require_equal(vm.get("name"), "s", "VM name")
    require_equal(vm.get("backend"), "crosvm", "VM backend")
    require_equal(vm.get("memory_mb"), 2048, "VM memory")
    require_equal(vm.get("auto_up"), False, "auto-up setting")
    require_equal(vm.get("protected_vm"), "pseudo_unprotected", "VM protection")
    require_equal(vm.get("gpu_enabled"), True, "GPU enabled setting")
    require_equal(vm.get("gpu_mode"), "native", "GPU mode")
    require_equal(vm.get("gpu_provider"), "drm2kgsl", "GPU provider")
    require_equal(vm.get("gpu_api"), "drm2kgsl", "GPU API")
    require_equal(vm.get("gpu_backend"), "gpu_virglrenderer", "GPU backend")
    require_equal(vm.get("gpu_udmabuf"), True, "GPU udmabuf setting")

    disks = vm.get("disks")
    if not isinstance(disks, list) or len(disks) != 1:
        raise RuntimeError("VM must contain exactly one disk")
    disk = disks[0]
    if not isinstance(disk, dict):
        raise RuntimeError("disk entry is not an object")
    require_equal(disk.get("bus"), "virtio", "disk bus")
    require_equal(disk.get("path"), ACTIVE_DISK, "active disk path")
    require_equal(disk.get("readonly"), False, "active disk readonly setting")

    environment = vm.get("environment_variables")
    if not isinstance(environment, list) or not all(
        isinstance(item, str) for item in environment
    ):
        raise RuntimeError("environment_variables must be a string array")
    if any(item.split("=", 1)[0] == "DROIDVM_SHIM_PARCEL_MB" for item in environment):
        raise RuntimeError("DROIDVM_SHIM_PARCEL_MB is already configured")
    return vm


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


def metadata(path: Path) -> tuple[int, int, int, str]:
    output = run(
        ["/system/bin/stat", "-c", "%u:%g:%a:%C", str(path)],
        capture=True,
    ).stdout.strip()
    fields = output.split(":", 3)
    if len(fields) != 4:
        raise RuntimeError(f"unexpected stat output for {path}: {output!r}")
    uid, gid, mode, context = fields
    return int(uid), int(gid), int(mode, 8), context


if os.getuid() != 0:
    raise SystemExit("must run as root")
if TEMP.exists():
    raise SystemExit(f"refusing to overwrite temporary file: {TEMP}")
if process_ids("crosvm"):
    raise SystemExit("refusing to edit vms.json while crosvm is running")
if not CONFIG.is_file() or not BACKUP.is_file():
    raise SystemExit("active config or verified backup is missing")

run(["/system/bin/sync"])
raw = CONFIG.read_bytes()
backup_raw = BACKUP.read_bytes()
require_equal(sha256(raw), EXPECTED_PREIMAGE_HASH, "active config hash")
require_equal(sha256(backup_raw), EXPECTED_PREIMAGE_HASH, "backup config hash")
if CONFIG.stat().st_ino == BACKUP.stat().st_ino:
    raise SystemExit("backup must be an independent file")

data = json.loads(raw.decode("utf-8"))
vm = validate_config(data)
if serialize(data) != raw:
    raise SystemExit("active config is not the expected canonical serialization")

updated = copy.deepcopy(data)
updated_vm = validate_config(updated)
updated_vm["environment_variables"].append(ENVIRONMENT_ENTRY)
candidate = serialize(updated)
candidate_data = json.loads(candidate.decode("utf-8"))
if candidate_data != updated:
    raise SystemExit("candidate JSON round trip changed its structure")

old_line = b'            "environment_variables": [],'
new_lines = (
    b'            "environment_variables": [\n'
    b'                "DROIDVM_SHIM_PARCEL_MB=256"\n'
    b'            ],'
)
if raw.count(old_line) != 1 or candidate != raw.replace(old_line, new_lines, 1):
    raise SystemExit("candidate contains changes outside environment_variables")

source_metadata = metadata(CONFIG)
app_was_stopped = False
edit_completed = False
try:
    run(["/system/bin/am", "force-stop", APP_PACKAGE])
    app_was_stopped = True
    for _ in range(20):
        if not process_ids(APP_PACKAGE):
            break
        time.sleep(0.25)
    else:
        raise RuntimeError("DroidVM app did not stop")

    if process_ids("crosvm"):
        raise RuntimeError("crosvm appeared after the preflight")
    if CONFIG.read_bytes() != raw:
        raise RuntimeError("active config changed during app shutdown")

    run(["/system/bin/cp", "--preserve=all", str(CONFIG), str(TEMP)])
    with TEMP.open("r+b", buffering=0) as output:
        written = output.write(candidate)
        require_equal(written, len(candidate), "candidate write length")
        output.truncate()
        os.fsync(output.fileno())
    run(["/system/bin/restorecon", str(TEMP)])
    require_equal(metadata(TEMP), source_metadata, "temporary-file metadata")
    require_equal(json.loads(TEMP.read_text(encoding="utf-8")), updated, "candidate data")

    os.replace(TEMP, CONFIG)
    run(["/system/bin/restorecon", str(CONFIG)])
    directory_fd = os.open(CONFIG.parent, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(directory_fd)
    finally:
        os.close(directory_fd)

    require_equal(metadata(CONFIG), source_metadata, "active-file metadata")
    require_equal(CONFIG.read_bytes(), candidate, "installed config bytes")
    require_equal(json.loads(CONFIG.read_text(encoding="utf-8")), updated, "installed data")
    require_equal(BACKUP.read_bytes(), backup_raw, "backup bytes")
    edit_completed = True
finally:
    if TEMP.exists():
        TEMP.unlink()
    if app_was_stopped:
        start = run(
            ["/system/bin/am", "start", "-W", "-n", APP_COMPONENT],
            capture=True,
        )
        print(start.stdout.strip())

if not edit_completed:
    raise SystemExit("config update did not complete")

installed = CONFIG.read_bytes()
print(f"preimage_sha256={EXPECTED_PREIMAGE_HASH}")
print(f"installed_sha256={sha256(installed)}")
print(f"backup_sha256={sha256(BACKUP.read_bytes())}")
uid, gid, mode, context = metadata(CONFIG)
print(f"installed_metadata={uid}:{gid}:{mode:o}:{context}")
print(f"environment={ENVIRONMENT_ENTRY}")
