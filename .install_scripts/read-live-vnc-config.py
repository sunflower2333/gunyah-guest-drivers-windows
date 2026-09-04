#!/usr/bin/env python3
import json
import subprocess
from pathlib import Path


result = subprocess.run(
    ["/system/bin/pidof", "crosvm"],
    check=False,
    text=True,
    capture_output=True,
)
if result.returncode != 0:
    raise SystemExit("crosvm is not running")
pids = result.stdout.split()
if len(pids) != 1:
    raise SystemExit(f"expected exactly one crosvm, found {pids}")

raw = (Path("/proc") / pids[0] / "cmdline").read_bytes()
args = [item.decode("utf-8") for item in raw.split(b"\0") if item]
indexes = [index for index, value in enumerate(args) if value == "--vnc-server"]
if len(indexes) != 1 or indexes[0] + 1 >= len(args):
    raise SystemExit("live crosvm has no unique VNC configuration")

fields = {}
for item in args[indexes[0] + 1].split(","):
    key, separator, value = item.partition("=")
    if separator:
        fields[key] = value
for required in ("host", "port"):
    if not fields.get(required):
        raise SystemExit(f"VNC configuration is missing {required}")

print(
    json.dumps(
        {
            "pid": int(pids[0]),
            "host": fields["host"],
            "port": int(fields["port"]),
            "password": fields.get("password", ""),
            "security": "VNCAuth" if fields.get("password") else "None",
        },
        separators=(",", ":"),
    )
)
