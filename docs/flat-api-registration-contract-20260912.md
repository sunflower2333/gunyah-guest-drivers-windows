# Installed flat API registration contract

`viogpu-api-registration.psm1` is a validation helper for the unified GPU
installer. It makes no registry, driver or device changes. The actual driver
binding and catalog/hash checks remain the lifecycle installer's responsibility.

Import this module alongside `viogpu-install-state.psm1`. After reading the
active adapter and all ten API registry values, call:

```powershell
Assert-VioGpuApiRegistration -Manifest $State.Manifest -StoreRoot $store `
    -DriverKey $actual.DriverKey -Snapshot $snap
```

Only then record `$State.InstalledApi` and `$State.Installed`. Separately reject
a non-OK PnP status or nonzero `DEVPKEY_Device_ProblemCode`; matched file identity
alone does not show that the selected device started.

The validator requires the signed manifest's exact six API filenames and the
same DriverStore directory for all six registry paths. OpenGL native and Wow
paths are single-element `REG_MULTI_SZ`; Vulkan/OpenCL use `REG_SZ`. Both
OpenGLVersion and OpenGLFlags, including their Wow counterparts, must be DWORD1.
It rejects duplicate/missing values, stale package paths, incorrect registry
views/adapter keys and registrations selecting a single-ABI native CL DLL.

The multi-string validator accepts both `string[]` from native registry reads
and `ArrayList` after CLIXML journal recovery, with exactly one string in either
case. The first local test run exposed this distinction; the corrected test
passes the actual PSSerializer roundtrip.

References reviewed locally:

- Microsoft `display/wddm-2-1-features.md`: DIRID13 and OpenGL REG_MULTI_SZ.
- Microsoft `display/loading-an-opengl-installable-client-driver.md`: OpenGL
  discovery validates version and consumes ICD flags as well as the DLL name.
- Khronos `loader/windows/icd_windows_hkr.c`: OpenCLDriverName and Wow discovery.

Validation: `test-viogpu-api-registration.ps1` has 31 contract cases. With
`-RegistryFixture`, it writes and reads real typed values in an isolated HKLM
test key, then removes that exact key. CI runs this in Windows PowerShell5.1
and PowerShell7 on x64 and ARM64. Portable local validation passes; Windows CI
must pass separately. These checks do not prove ICD execution or GPU rendering.
