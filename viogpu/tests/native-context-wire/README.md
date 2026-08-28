# Native Context Wire ABI Fixture

`abi_manifest_entries.h` is the version-2 canonical manifest for the DRM/MSM
native-context boundary. Each entry has an expected numeric value, so a compiler
fails before execution if any opcode, `sizeof`, or `offsetof` changes.

The fixture covers the DRM capset MSM payload, `vdrm_shmem`, request/response
headers, all 11 MSM ccmd request/response prefixes, the optional arena-v2 run
list, and the VirtIO-GPU commands and flags used to carry native-context data.
The Windows endpoint also exercises the complete `RESOURCE_MAP_BLOB` response
classifier, including the 32-byte cached success shape, ordinary 24-byte host
errors, incomplete/short responses, and malformed map-info or padding fields.

The Host capset container can be larger than the MSM payload because its union
also contains other backends. The compared invariant is the MSM payload extent:
88 bytes at offset 24, ending at byte 112. Windows intentionally stores only
that MSM prefix.

Run GCC, Clang, Windows-header Clang, and the Guest AArch64 compile gate:

```sh
./run-local.sh
```

Run the real MSVC gate from an initialized Visual Studio command prompt:

```bat
run-msvc.cmd
```

The AArch64 executable should also be run inside the Linux Guest and its output
compared byte-for-byte with `expected-v2.txt`; cross compilation alone proves
layout assertions but is not a substitute for the Guest runtime manifest.
