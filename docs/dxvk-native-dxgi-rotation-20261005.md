# Native DXGI rotation candidate

The package pins DXVK to
`4f59adf77cf3d6207c85af85f7a39adff3d8e367`, which implements the production
`DXGI_DDI_BASE_FUNCTIONS::pfnRotateResourceIdentities` callback. Resource
handles remain stable while allocation identities rotate. Private resources
retain their backend objects and views; shared resources publish dirty pixels
to their old backing before rotation and invalidate their cached pixels.
Validation precedes identity changes, and callback retirement and nested
rotation are covered by the regression.

DXGI coverage is now 2/7. Admission remains closed with the same eight gaps;
the flat INF continues to register Mesa and ships DXVK/VKD3D as unregistered
candidates. Native DX8/DX9 support and ordinary Windows runtime/hardware
acceptance remain unfinished.

Source CI 37310474949 passed the offline contracts, x64/x86 production DDI
regression and ARM64 compilation. Full source CI 37310476652 passed all
embedded backend architecture builds and native ARM64 runtime fixtures. These
functional fixtures use Microsoft WARP and do not establish Turnip hardware
acceptance. The paired wrapper now explicitly copies
`dxvk-umd-rotation-test.exe` for the pinned tree's native ARM64 fixture runner;
x64/x86 run it inside the source build script.

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
