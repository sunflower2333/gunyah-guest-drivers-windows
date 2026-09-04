#!/bin/sh
# Cross-build the viogpud3d interposer for ARM64 Windows on a Linux host.
# It forwards the D3D10/11 user-mode driver entry points to the real Mesa build
# and records the DDI negotiation, so the runtime's side of a failed
# D3D11CreateDevice is visible without a kernel debugger.
#
# Needs only clang with lld; no WDK and no CRT.
set -eu
cd "$(dirname "$0")"
llvm-dlltool -m arm64 -d viogpud3d-interpose-kernel32.def -l kernel32.lib
clang --target=aarch64-windows-msvc -shared -nostdlib -O2 \
      -o viogpud3d-interpose.dll viogpud3d-interpose.c kernel32.lib \
      -Wl,-entry:DllMain -fuse-ld=lld
llvm-readobj --file-headers viogpud3d-interpose.dll | grep -i machine
