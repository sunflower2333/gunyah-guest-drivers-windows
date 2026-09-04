#!/usr/bin/env python3
import argparse
import json
import socket


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--interface", default="vma5b6863f-0")
    parser.add_argument("--address", default="fe80::bdf3:be12:d46f:7a8d")
    parser.add_argument("--port", type=int, default=22)
    parser.add_argument("--timeout", type=float, default=8.0)
    return parser.parse_args()


args = parse_args()
interface_index = socket.if_nametoindex(args.interface)

with socket.socket(socket.AF_INET6, socket.SOCK_STREAM) as connection:
    connection.settimeout(args.timeout)
    connection.setsockopt(
        socket.SOL_SOCKET,
        getattr(socket, "SO_BINDTODEVICE", 25),
        args.interface.encode("ascii") + b"\0",
    )
    connection.connect((args.address, args.port, 0, interface_index))
    banner = connection.recv(512)

print(
    json.dumps(
        {
            "interface": args.interface,
            "interface_index": interface_index,
            "address": args.address,
            "port": args.port,
            "banner": banner.decode("ascii", "replace").strip(),
        },
        separators=(",", ":"),
    )
)
