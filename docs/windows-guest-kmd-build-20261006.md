# Native ARM64 KMD build in the Windows guest

The existing Windows VM successfully built `viogpuwddm.sys` from exact paired
source `b6bf4c4f849f5ca597637fc2ce591f23a826f158`. This provides a quick KMD
compiler feedback path alongside the signed CI pipeline. Installed driver
`100.6.101.58623`, DWM PID 1644 and Explorer PID 5828 were preserved.

## Toolchain and source

Guest directory: `C:\Users\Public\VioGpuKmd-b6bf4c4-20261006`.
Tools are extracted under its `tools` directory; no VS/SDK/WDK installer,
registry change, reboot or VM configuration change was needed. All compiler,
linker, resource and WPP tools are native ARM64, rather than emulated x86.

- MSVC compiler `19.44.35229.0`, linker `14.44.35229.0`, from Microsoft's
  VS17 catalog. The package uses internal directory `14.44.35207`; actual
  executable versions are recorded separately.
- SDK/WDK packages `10.0.26100.1`, kit layout `10.0.26100.0`.
- `Microsoft.Windows.SDK.BuildTools` supplies native `rc.exe`/`tracewpp.exe`;
  the SDK headers package supplies WPP configuration templates.
- `Microsoft.Windows.SDK.CPP.arm64` supplies `arm64rt.lib` in its `um/arm64`
  directory. This library is absent from the compiler and WDK ARM64 packages.
- Source archive contains 294 files from `build`, `VirtIO` and `viogpu`;
  its files match the exact Git blobs. The runner checks all source hashes
  before and after the build.

The direct commands reproduce successful paired CI
[37338579561](https://github.com/sunflower2333/gunyah-guest-drivers-windows/actions/runs/37338579561).
They compile plain VirtioLib's five C files, generate WPP for the ten KMD C++
files with the separate non-owner `driver.cpp` template, compile the version
resource, and link the kernel driver. Release `/GL`, `/LTCG`, `/kernel`, `/GS`,
`/guard:cf`, `/WX`, `GsDriverEntry` and native subsystem are retained, as are
the advanced-color/FP16/WDDM2.3/connection-DDI flags and pipeline window 64.
MPO3 and test implementations are disabled. MSBuild itself is not installed.

Kernel CRT headers must precede MSVC headers. WPP and RC receive direct
arguments; compiler/librarian/linker commands use response files. These choices
resolve the observed header collision and RC response-file output quoting.

## Measured results

All guest intervals use `Stopwatch`, independent of wall-clock adjustments.

| Phase | Guest duration |
| --- | ---: |
| Portable archive extraction, hash checks and tool validation | 51.224 s |
| Additional SDK ARM64 support-library validation/extraction | 0.269 s |
| VirtIO compile and librarian | 0.648 s |
| KMD WPP, compilation, resource and link | 4.113 s |
| Entire build runner, including source/PE checks | 5.114 s |

The base payload transfer was 26.655 s; package downloads, local archive
assembly and the SDK ARM64 supplement transfer are separate provisioning
costs, not included in the extraction or compile durations above.

CI's corresponding KMD WPP-to-SYS interval was 4.391 s, its plain VirtIO
compiler/librarian interval 6.222 s, and its KMD project reported 6.55 s.
The complete paired workflow took 19m50s and includes Mesa/DXVK prerequisites,
all drivers, package signing and runtime validation. Guest compilation avoids
that turnaround for KMD-only edits; it does not replace those package gates.
The CI compiler ran HostX86-to-ARM64 on the x64 VS2022 runner, whereas this
guest used HostARM64-to-ARM64. This is one measured build, not a benchmark.

Output `viogpuwddm.sys`: 329216 bytes, version `100.6.101.58522`, SHA256
`7c0a45f2bbf3424ff8a67c4c6c2369d13e329331c5e4adf46ab3b74d196e934b`.
Local LLVM inspection confirms ARM64/native subsystem, version resource,
CFG/NX/ASLR flags, security cookie and kernel-only NT/HAL imports. The link map
confirms `GsDriverEntry`. SYS/PDB/map hashes agree with the guest receipt;
every tool exited zero and all tool stderr files are empty.

## Reuse and evidence

The existing provisioned directory can rebuild this exact source with:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File C:\Users\Public\VioGpuKmd-b6bf4c4-20261006\build-guest-kmd.ps1
```

The runner deliberately requires its exact source receipt and version. For a
new source checkpoint, generate a fresh Git archive/manifest, use an isolated
source directory, and update the expected source/version in the recipe while
reusing the portable tools. Preserve this successful receipt before another
run overwrites `build-result.json` and `build-logs`.

Workspace evidence:
`artifacts/dxvk-native-rotation-20261005/guest-kmd-b6bf4c4/` contains verified
output, source/toolchain manifests, setup/build receipts, response files,
stdout/stderr, failed bootstrap attempts and successful script snapshots.
`build-verified.json` binds downloaded bytes to the guest output hashes.

This SYS is unsigned and uninstalled. It does not establish package admission,
KMD load acceptance, native DX8/DX9, ordinary DX10/DX11 activation or Turnip
hardware rendering. The active desktop remains on the newer installed stack.
