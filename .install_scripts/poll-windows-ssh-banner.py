#!/usr/bin/env python3
import argparse
import json
import socket
import time
from datetime import datetime, timezone


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--address", default="192.168.60.237")
    parser.add_argument("--port", type=int, default=22)
    parser.add_argument("--attempts", type=int, default=30)
    parser.add_argument("--interval", type=float, default=10.0)
    parser.add_argument("--timeout", type=float, default=5.0)
    return parser.parse_args()


args = parse_args()
for attempt in range(1, args.attempts + 1):
    started = time.monotonic()
    result: dict[str, object] = {
        "attempt": attempt,
        "time": datetime.now(timezone.utc).isoformat(),
    }
    try:
        with socket.create_connection(
            (args.address, args.port), timeout=args.timeout
        ) as connection:
            connection.settimeout(args.timeout)
            banner = connection.recv(512).decode("ascii", "replace").strip()
        result["banner"] = banner
        print(json.dumps(result, separators=(",", ":")), flush=True)
        if banner.startswith("SSH-"):
            raise SystemExit(0)
    except OSError as error:
        result["error"] = f"{type(error).__name__}: {error}"
        print(json.dumps(result, separators=(",", ":")), flush=True)

    if attempt != args.attempts:
        remaining = args.interval - (time.monotonic() - started)
        if remaining > 0:
            time.sleep(remaining)

raise SystemExit(1)
