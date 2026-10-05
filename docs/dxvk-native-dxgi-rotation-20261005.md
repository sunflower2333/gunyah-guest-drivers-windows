# Native DXGI rotation candidate

The package pins DXVK to
`34ff484ac87b66f77e1c9f6d8db914783b24788e`, which implements the production
`DXGI_DDI_BASE_FUNCTIONS::pfnRotateResourceIdentities` callback. Resource
handles remain stable while allocation identities rotate. Private resources
retain their backend objects and views; shared resources publish dirty pixels
to their old backing before rotation and invalidate their cached pixels.
Validation precedes identity changes, and callback retirement and nested
rotation are covered by the regression.

The runtime owns the DXGI callback table; Present reads its current callback
slot. Presentation separately retains allocation, readback texture and backend
owners across callbacks that can retire and poison resource private storage.
A registry reservation cancels submission after identity or context callbacks
retire the source. Release waits until the operation unwinds. Device-wide
exclusion rejects nested Present and rotation while presentation is active,
and rejects Present during rotation. The existing rotation and lifetime
fixture names cover these cases, so their ARM64 handoff remains unchanged.

DXGI coverage is now 2/7. Admission remains closed with the same eight gaps;
the flat INF continues to register Mesa and ships DXVK/VKD3D as unregistered
candidates. Native DX8/DX9 support and ordinary Windows runtime/hardware
acceptance remain unfinished.

The earlier rotation source `4f59adf` passed source CI 37310474949: offline
contracts, x64/x86 production DDI
regression and ARM64 compilation. Full source CI 37310476652 passed all
embedded backend architecture builds and native ARM64 runtime fixtures. These
functional fixtures use Microsoft WARP and do not establish Turnip hardware
acceptance. The paired wrapper now explicitly copies
`dxvk-umd-rotation-test.exe` for the pinned tree's native ARM64 fixture runner;
x64/x86 run it inside the source build script.

The callback-table source `af84b74` subsequently passed offline CI 37335070338
and full source CI 37335069804, including native ARM64 fixture execution.
The current Present ownership source `34ff484` passed independent review,
the 101-slot WDK loadability policy, and local ASan/UBSan suites with 69
identity, 180 runtime-identity and 147 shader checks. Exact offline source
CI 37338141939 passed all four jobs; full native CI 37338144307 passed all
six, including ARM64 runtime execution. Paired CI 37338579561 passed all
three DXVK architectures and native ARM64 functional fixtures: lifetime
10834 checks and rotation 1428 checks/44 synchronized locks.

All 73 existing package unit tests passed, and `git diff --check` passed.
Run the direct `build-arm64-drivers.yml`
workflow to validate the paired candidate's architecture loading, signing,
catalog and source receipts. The workflow reserves version 100.6.101.58522
and keeps its existing Mesa pins: D3D
`eae74a860208698428227d89d05bb63d274a8853` and GL/Turnip
`3e50dd4ba4f941fcb4ddeb91d0cbd688cabfdb4a` (Mesa CI 37312838033).

This is an older package baseline than active installed driver 58623 with
Mesa `b36366b53c17cc55c84df7b756b45fb6e4e4e8fa`. Its artifacts serve as
candidate integration evidence; this checkpoint does not authorize replacing
the active desktop stack.

Paired CI 37318070394 passed the three DXVK architecture builds, all Mesa
and API prerequisites, and the native ARM64 rotation regression (1281
checks/43 locks). Driver compilation stopped before source compilation:
the migrated Windows ARM64 image pairs VS18 with WDK26100, whose build task
assembly supports VS17. The workflow now cross-builds the ARM64 driver
targets on the matching VS2022/x64 host. Native DXVK functional fixtures
remain mandatory in a Windows ARM64 job before packaging. Actual catalog,
file hashes, common signer and installer receipt are checked on the build
host; all signed ARM64/EC/x86 runtime load checks remain mandatory in a
separate Windows ARM64 job after packaging. GPU source and package pins do
not change for this CI repair. Replacement CI 37336134109 exposed an inherited
literal runner assertion; the checker now enforces the per-job runners,
dependencies and full ARM64 load checks. That diagnosed run was cancelled as
superseded before integrating the current Present ownership source.

Paired CI 37338579561 completed successfully, all 16 jobs. The product
cross-build and full Windows ARM64 runtime job each verified 53 actual catalog
members with the common valid signer. The installer accepted the signed
52-file manifest. Native ARM64, emulated x64/EC and x86 processes loaded the
signed GL, CL and D3D payloads, plus DXVK candidates through their exact
private Vulkan loaders; OpenAdapter10 still returned the closed gate.

The final artifact is `11358925550`, produced from driver commit
`b6bf4c4f849f5ca597637fc2ce591f23a826f158`, version `100.6.101.58522`.
The downloaded ZIP SHA256 is
`1aad40d95c607738fe1e36b64488c7b4ce50e2010e77d3fb1f103db875f8f919`, matching
the upload record. Local verification of all 64 GPU and nine installer files,
PE architectures, source pins, exact eight-gap admission set and signed file
hashes reproduced the uploaded joint receipt exactly. The extracted signed
package, catalog, manifest, source/native receipts and Windows load logs are
retained in workspace
`artifacts/dxvk-native-rotation-20261005/paired-ci-37338579561/`.
Later documentation commits do not change that binary producer identity.
