# Unified GPU installer integration

The normal `INSTALL.cmd` / `install-drivers.ps1` entry now sends the GPU package
through `viogpu-unified-install.ps1`. This installs the KMD, D3D UMD and the flat
Vulkan/OpenGL/GLES/OpenCL payload as one driver package. It does not copy new
API runtimes to Program Files. Normal installation of the other driver classes
keeps its existing ordering.

The installer resolves the exact published INF and DriverStore directory,
verifies the signed catalog and every inventoried file, and checks the actual
GPU binding and all native/Wow API registrations. It preserves the prior driver
package for explicit rollback or failed-install recovery. Concurrent unrelated
driver changes are preserved. A successful rollback of a previously healthy
adapter also requires the restored adapter's PnP status to be OK with problem0.

GPU-only rollback and uninstall use the saved protected journal:

```powershell
.\install-drivers.ps1 -GpuAction Rollback -GpuJournalPath <saved-state.clixml>
.\install-drivers.ps1 -GpuAction Uninstall -GpuJournalPath <saved-state.clixml>
```

Before driver staging, the existing test-certificate installation behavior now
checks the bundled public certificate against the catalog's exact signer and
verifies its cryptographic signature. It records newly created trust entries
and removes only those entries if package verification fails before staging.
Existing trust entries and other publishers remain untouched.

The final artifact must include these flat installer-root files:

- INSTALL.cmd and install-drivers.ps1
- viogpu-unified-install.ps1
- viogpu-install-state.psm1
- viogpu-install-native.cs
- viogpu-api-registration.psm1
- viogpu-install-certificates.psm1
- DroidVM_Test.cer, matching the signed GPU catalog
- Existing pvmpower-devnode.ps1 for the normal multi-driver entry

The GPU payload manifest must also contain `loader_probes` mapping arm64/x64/x86
to `opencl-loader-check-<arch>.exe`. These three installed helpers belong in the
same flat signed DriverStore payload. Other CI probe files remain excluded.

Source integrated here: installer worker
`bca1712e58730a67081620a8149c4ead1d6ed561`, based on58473/8b51db7. Root's original
four registration helper/test files are identical to the worker's copies.

Validation at this integration point:

- Six PowerShell scripts/modules parse and the native C# helper compiles locally.
- Registration CI34699829545 passes31 cases on each x64/ARM64 and PS5.1/PS7 pair.
- Final lifecycle CI34701031575 passes105 lifecycle/catalog/trust checks and31
  real-registry API checks on each x64 and ARM64 runner.
- The original ARM64 failure was caused by opening only existing certificate
  stores on a fresh Windows installation. Logs confirmed the physical
  TrustedPublisher store was absent. Opening or creating the system store with
  its normal API flags fixes insertion while preserving ACLs and policy.

This is source integration, not a deployable full package. Final helper bundling,
flat signed manifest composition and real GPU
installation/rendering validation are still required.
