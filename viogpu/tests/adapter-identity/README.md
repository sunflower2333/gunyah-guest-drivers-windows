# Native runtime adapter identity

The native Direct3D runtime adapter handle is opaque. The UMD needs the
Windows-assigned adapter LUID from the miniport callback to select the same
Vulkan physical device without casting that handle or guessing an index.

The miniport captures `DXGK_START_INFO.AdapterLuid` after successful start.
Stop and failed-start unwind clear the atomic identity before draining
hardware operations. An ordinary GPU reset preserves the adapter LUID, but
queries fail while hardware is not ready. The optional identity read occurs
inside the existing hardware rundown window; a stop invalidation racing
that read is rejected. A new PnP start supplies its new OS-assigned identity.

`viogpu_wddm_abi.h` and its v0 128-byte adapter reply are unchanged. An output
buffer of at least 160 bytes receives an additional 32-byte trailer defined
separately in `viogpu_adapter_identity.h`. `Header.Size` remains 128; 129–159
byte callers receive only the legacy prefix. Flags/magic/version/node mask
match the DXVK optional identity decoder: VLID `0x44494c56`, version 1, size
32, valid flag 1, exact 8-byte LUID, node mask 1, reserved zero. No allocation,
submission ABI, GPU VA, resource identity or runtime registration changes.

Run `python viogpu/tests/adapter-identity/run.py`. The 198 checks compile the
production publication/readiness/private-query functions with controlled OS
peers and real wire definitions. They cover every output length 0–176,
oversized declared output, unchanged prefix, malformed requests, no output
writes on failure, zero/missing LUID, reset versus new-start identity,
hardware teardown/readiness failures and a stop during the protected query.
PnP call ordering is checked against the actual StartDevice/StopDevice/unwind
source. These tests do not substitute for Windows PnP/runtime validation.

Local ASan/UBSan: 198/198 PASS. The old 7648b72f private reply used as a
negative control fails 23/198. Existing KMD/UMD ABI manifests pass with GCC
and Clang, and the full miniport source contract passes. That contract retains
the original nonpaged ranges and now checks the bounded identity branch.
Full WDK build and paired DXVK integration are the next gates.

Microsoft documents AdapterLuid as the locally unique identifier of the
adapter being started, available since Windows 8:
https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/dispmprt/ns-dispmprt-_dxgk_start_info

This is an adapter-selection prerequisite. It does not by itself implement
the remaining native Direct3D DDIs or prove GPU rendering.
