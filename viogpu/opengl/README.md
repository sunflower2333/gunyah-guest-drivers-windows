# Flat Windows GL/Vulkan/GLES runtime

The runtime connects Microsoft's OpenGL ICD interface to Mesa Zink and Turnip.
All architecture DLLs share the graphics driver directory. Mesa's app-local
`opengl32.dll` is a build probe dependency only and is excluded from this payload.

| Caller on Windows ARM64 | ICD entry | Actual backend |
| --- | --- | --- |
| Native ARM64 | `viogpuopengl.dll`, ARM64X native view | `viogpu_gl_arm64.dll` |
| x64/EC | `viogpuopengl.dll`, ARM64X EC view | `viogpu_gl_x64.dll` |
| x86 | `viogpuopengl_x86.dll` | `viogpu_gl_x86.dll` |

The ARM64X binary combines adapter code for two ABIs. It does not merge the
complete Mesa backends: the x64 backend remains AMD64 code. An actual backend
ARM64X build needs both native and ARM64EC object/import-library closures and
separate per-view exports. Mesa's existing ARM64EC build and CHPE verifier
provide a starting point; they are not full backend ARM64X runtime proof.

For each `arm64`, `x64`, `x86`, private runtime files are:

- `viogpu_gl_<arch>.dll`: Gallium WGL ICD and shared GL dispatch
- `viogpu_egl_<arch>.dll`, `viogpu_gles1_<arch>.dll`, `viogpu_gles2_<arch>.dll`
- `viogpu_gl_vk_<arch>.dll`: Turnip Vulkan ICD
- `viogpu_gl_loader_<arch>.dll`: private Khronos Vulkan loader

Meson and the pinned loader's CMake/module definitions assign these names before
linking. EGL/GLES import their exact architecture's Gallium DLL. Zlib is static,
so there is no shared `z-1.dll`. The proxy and Zink resolve private dependencies
from their own directory, reject same-name modules from another package, and
can coexist with an application's public `vulkan-1.dll`.

`turnip.json` and `turnip-wow.json` reference same-directory native/ARM64X and
x86 proxies. The driver INF owns device-scoped GL/Vulkan registration. No global
Khronos key or PATH override is needed. Windows has no system EGL/GLES ICD
registration contract; clients must select these explicit private DLL paths.

`package.py` verifies the pinned source/run, all input hashes, PE architecture,
real ICD exports and actual import descriptors including delay imports. It
rejects generic private dependencies, foreign-architecture imports and extra
input files. `flat-runtime.json` schema 1 records every payload file's hash,
machine and role; the main driver composer stages runtime/ICD files into the
same INF/catalog. Probes are CI outputs, not installed driver payload.

The OpenGL workflow builds and executes actual ARM64, ARM64X x64-view and x86
ICD/EGL/GLES load probes on Windows ARM64. Vulkan negotiation and GL version
validation run twice on 64 KiB reserved worker stacks. A current-source control
restores the former automatic 128 KiB path storage and must fail specifically
with `STATUS_STACK_OVERFLOW`. Missing dependencies, wrong architecture DLLs and
foreign-package modules must fail; a preloaded public Vulkan loader must pass.

These calls create no graphics context or GPU workload. Real system WGL,
GLES1/2 triangle readback/presentation, Vulkan applications, desktop stability
and stress acceptance still require the target device with the final signed
driver package. The root workflow owns final INF, signing and installation.

References:

- https://learn.microsoft.com/en-us/windows-hardware/drivers/display/loading-an-opengl-installable-client-driver
- https://learn.microsoft.com/en-us/windows/arm/arm64ec
- https://learn.microsoft.com/en-us/windows/arm/arm64x-build
- https://raw.githubusercontent.com/KhronosGroup/Vulkan-Loader/vulkan-sdk-1.4.304.0/docs/LoaderDriverInterface.md
