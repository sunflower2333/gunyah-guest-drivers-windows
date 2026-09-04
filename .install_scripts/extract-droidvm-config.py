#!/usr/bin/env python3

import json
import sys
from pathlib import Path


if len(sys.argv) != 4:
    raise SystemExit("usage: extract-droidvm-config.py LIST_JSON VM_ID OUTPUT_JSON")

source = Path(sys.argv[1])
vm_id = sys.argv[2]
output = Path(sys.argv[3])
payload = json.loads(source.read_text(encoding="utf-8"))
matches = [vm for vm in payload.get("data", []) if vm.get("id") == vm_id]
if len(matches) != 1:
    raise RuntimeError(f"expected one VM {vm_id}, found {len(matches)}")

config = dict(matches[0])
config.pop("pid", None)
config.pop("state", None)
output.write_text(json.dumps(config, separators=(",", ":")), encoding="utf-8")
