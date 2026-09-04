#!/usr/bin/env python3
import json
import re
import subprocess
import time
from pathlib import Path


ACTIVE_DISK = "/mnt/pass_through/0/emulated/0/win11-droidvm-final-comp.qcow2"
ENVIRONMENT_ENTRY = "DROIDVM_SHIM_PARCEL_MB=256"
APP_CACHE = Path("/data/data/cn.classfun.droidvm/cache")
VM_ID = "a5b6863f-6bbf-4af4-97f3-2a4653f6edb7"
PAYLOAD_GPA = 0x80600000
PAYLOAD_SIZE = 0x7F200000
CHUNK_SIZE = 0x10000000


def pidof(name: str) -> list[int]:
    result = subprocess.run(
        ["/system/bin/pidof", name],
        check=False,
        text=True,
        capture_output=True,
    )
    if result.returncode not in (0, 1):
        raise RuntimeError(f"pidof {name} failed with {result.returncode}")
    return [int(item) for item in result.stdout.split()]


def nul_fields(path: Path) -> list[str]:
    raw = path.read_bytes()
    return [item.decode("utf-8") for item in raw.split(b"\0") if item]


def option(args: list[str], name: str) -> str:
    matches = [index for index, value in enumerate(args) if value == name]
    if len(matches) != 1 or matches[0] + 1 >= len(args):
        raise RuntimeError(f"expected exactly one {name} option")
    return args[matches[0] + 1]


if subprocess.run(["/system/bin/id", "-u"], capture_output=True, text=True).stdout.strip() != "0":
    raise SystemExit("must run as root")

pids = pidof("crosvm")
if len(pids) != 1:
    raise SystemExit(f"expected exactly one crosvm, found {pids}")
pid = pids[0]
proc = Path("/proc") / str(pid)
args = nul_fields(proc / "cmdline")
environment = nul_fields(proc / "environ")

environment_count = environment.count(ENVIRONMENT_ENTRY)
droidvm_environment_keys = sorted(
    item.split("=", 1)[0] for item in environment if item.startswith("DROIDVM_")
)
if option(args, "--mem") != "2048":
    raise SystemExit("live crosvm memory is not 2048 MiB")
if option(args, "--hypervisor") != "gunyah":
    raise SystemExit("live crosvm is not using Gunyah")
if option(args, "--pre-alloc") != "drm-host-mb=8":
    raise SystemExit("live crosvm does not have the expected 8 MiB host preallocation")
if "--protected-vm-pseudo-unprotected" not in args:
    raise SystemExit("live crosvm is not pseudo-unprotected")

name = option(args, "--name")
block = option(args, "--block")
gpu = option(args, "--gpu")
if name != "s":
    raise SystemExit(f"unexpected VM process name: {name}")
if block.split(",", 1)[0] != ACTIVE_DISK:
    raise SystemExit(f"unexpected active disk: {block}")
for required in (
    "virglrenderer",
    "context-types=drm",
    "udmabuf=true",
    "fixed-blob-mapping=true",
    "pci-bar-size=8388608",
):
    if required not in gpu:
        raise SystemExit(f"missing GPU option: {required}")

firmware = args[-1]
if not firmware.endswith(".fd"):
    raise SystemExit(f"unexpected firmware argument: {firmware}")

expected_parcels = []
remaining = PAYLOAD_SIZE
gpa = PAYLOAD_GPA
while remaining:
    size = min(remaining, CHUNK_SIZE)
    expected_parcels.append((gpa, size))
    gpa += size
    remaining -= size
if len(expected_parcels) != 8:
    raise SystemExit("internal parcel expectation is not eight entries")

stderr_path = APP_CACHE / f"console_{VM_ID}_stderr.log"
share_pattern = re.compile(
    r"GUNYAH-SHARE-BLOB: gpa=0x([0-9a-fA-F]+) "
    r"size=0x([0-9a-fA-F]+).*handle=0x[0-9a-fA-F]+"
)
summary = (
    "GH-SHIM: window 0x80600000+0x7f200000 shared as 8 parcel(s)"
)

deadline = time.monotonic() + 30
text = ""
while time.monotonic() < deadline:
    if len(pidof("crosvm")) != 1:
        raise SystemExit("crosvm exited while waiting for parcel evidence")
    if stderr_path.is_file():
        text = stderr_path.read_text(encoding="utf-8", errors="replace")
        found = {(int(gpa_hex, 16), int(size_hex, 16)) for gpa_hex, size_hex in share_pattern.findall(text)}
        if all(parcel in found for parcel in expected_parcels) and summary in text:
            break
    time.sleep(0.5)
else:
    raise SystemExit("timed out waiting for eight successful shim parcels")

if "GUNYAH-SHARE-BLOB: gpa=0x80600000 size=0x7f200000" in text:
    raise SystemExit("fresh log still contains the unsplit payload share")
if "Out of memory (os error 12)" in text:
    raise SystemExit("fresh log contains the previous ENOMEM failure")

matching_lines = [
    line
    for line in text.splitlines()
    if (
        "GUNYAH-SHARE-BLOB:" in line
        and any(
            f"gpa=0x{parcel_gpa:x} size=0x{parcel_size:x}" in line
            for parcel_gpa, parcel_size in expected_parcels
        )
    )
    or summary in line
]

print(
    json.dumps(
        {
            "pid": pid,
            "crosvm_count": len(pids),
            "name": name,
            "memory_mb": int(option(args, "--mem")),
            "environment_expected": ENVIRONMENT_ENTRY,
            "environment_count_in_proc": environment_count,
            "droidvm_environment_keys_in_proc": droidvm_environment_keys,
            "block": block,
            "gpu": gpu,
            "firmware": firmware,
            "stderr": str(stderr_path),
            "parcel_count": len(expected_parcels),
            "parcel_ranges": [
                f"0x{parcel_gpa:x}+0x{parcel_size:x}"
                for parcel_gpa, parcel_size in expected_parcels
            ],
        },
        separators=(",", ":"),
    )
)
for line in matching_lines:
    print(line)
