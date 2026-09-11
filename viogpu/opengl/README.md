# System OpenGL ICD candidate

This package connects Microsoft's system OpenGL runtime to the genuine Mesa
Zink ICD and Turnip. It does not replace system `opengl32.dll`. EGL/GLES DLLs
are also included in each architecture directory; Windows has no equivalent
system-wide EGL/GLES ICD registration contract.

## Architecture and discovery

| Calling process on ARM64 Windows | Adapter value | Actual implementation |
| --- | --- | --- |
| Native ARM64 | OpenGLDriverName -> viogpuopengl.dll | ARM64X native view -> viogpuopengl_arm64.dll -> arm64/libgallium_wgl.dll |
| AMD64 x64, including EC callers | OpenGLDriverName -> viogpuopengl.dll | ARM64X x64 view -> viogpuopengl_x64.dll -> x64/libgallium_wgl.dll |
| 32-bit x86 | OpenGLDriverNameWow -> viogpuopengl_x86.dll | x86/libgallium_wgl.dll |

The small native/x64/x86 adapters delegate all 19 `Drv*` entries using Mesa's
own `gldrv.h` declarations. The x64 implementation is a real AMD64 binary.
The ARM64X pure forwarder is linked from separate ARM64 and EC objects with
separate native/x64 export definitions. It is not a renamed ARM64 DLL.

Native and Wow `VulkanDriverName` entries point to package manifests. The
native manifest uses the same ARM64X dispatch; the Wow manifest uses the x86
adapter. Three Vulkan ICD interface exports delegate to real Turnip.
No `VK_DRIVER_FILES`, PATH change, global Khronos key or elevation-sensitive
environment override is installed.

The GL adapter explicitly preloads its private Vulkan loader before Zink's
basename lookup. It rejects an already-loaded same-basename dependency from
another directory. This prevents accidental package substitution but means
applications preloading another `vulkan-1.dll` or `z-1.dll` need compatibility
evaluation. Direct Vulkan negotiation does not require the private loader.

## Build and package evidence

`opengl-system-icd.yml` verifies all 39 hashes in each exact Mesa artifact from
run 34595644367, commit `6144b82eabd05ccf56d9201e64455f05f562883b`.
It checks PE machines and real (not forwarded) Mesa ICD exports, builds the
dispatch adapters, and executes load-only calls from all three architectures
on a Windows ARM64 runner. The ARM64X native and x64 views have both passed
actual `DrvValidateVersion` calls in CI 34590946886. These calls do not create
a graphics context and are not evidence of system OpenGL rendering.

The parent `build-arm64-drivers.yml` waits for this ABI gate and its existing
Mesa D3D UMD/KMD build. It signs every OpenGL sidecar PE with the same certificate
as the base driver, adds a binding to the exact signed KMD/UMD hashes, then
creates and signs a full-tree file catalog. It repeats actual ARM64/x64/x86
load-only execution on the final signed files. Original Mesa input hashes and
post-signing output hashes remain separately identified. CI tests catalog
verification/tamper rejection and typed registry restoration after partial
write failure.

## Controlled registration

The driver bundle includes `install-opengl-icd.ps1`,
`opengl-registration.psm1`, and `opengl/{payload,opengl.cat}`. Registration is
explicit and does not occur in the ordinary base-driver installer.

First install/trust the jointly built base driver using its existing workflow.
Inspect the exact VIOGPU instance ID. From an elevated 64-bit PowerShell in
the extracted bundle, run:

```powershell
./install-opengl-icd.ps1 -Action Verify
./install-opengl-icd.ps1 -Action Install -InstanceId '<exact PCI instance ID>'
```

The installer requires a trusted catalog and matching PE signers, an active
VIOGPU display adapter bound to `VioGpuWddm`, and the exact signed KMD service
file hash in the package binding. This checks the registered on-disk binary,
not the already-loaded kernel image. It copies the verified package to a new versioned
`Program Files/DroidVM/OpenGL` directory, verifies it again, saves all eight
original adapter values and types, then writes and reads back the new values.
On write failure it restores the previous state. It does not restart the VM,
remove driver packages, replace Windows DLLs, or delete existing package files.

Use the printed backup path to restore registration:

```powershell
./install-opengl-icd.ps1 -Action Rollback -InstanceId '<same PCI instance ID>' -BackupPath '<printed path>'
```

Rollback refuses to overwrite values changed by a subsequent installer.
Installed files remain available for recovery. Driver reinstall/update may
reset adapter values; verify registration and matching KMD binding again.

## Device validation still required

After registration, start a new interactive process using
`system-probe-{arm64,x64,x86}.exe --system <installed payload directory>`.
The probe verifies Microsoft's runtime path, creates its context through GDI
and system WGL, requires Zink/Turnip, checks red triangle pixels and swapping,
and verifies the actual Mesa ICD path. Use the existing bounded console-task
launcher and coordinated Host capture, followed by desktop stability checks.
Do not run it during the user's remote backup pause. No system registration,
system rendering, GLES target rendering or FurMark acceptance has yet been
proved by this integration work.

References:

- https://learn.microsoft.com/en-us/windows-hardware/drivers/display/loading-an-opengl-installable-client-driver
- https://learn.microsoft.com/en-us/windows/arm/arm64ec
- https://learn.microsoft.com/en-us/windows/arm/arm64x-build
- https://raw.githubusercontent.com/KhronosGroup/Vulkan-Loader/vulkan-sdk-1.4.304.0/docs/LoaderDriverInterface.md
