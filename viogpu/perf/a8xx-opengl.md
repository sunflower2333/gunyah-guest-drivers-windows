# A8xx OpenGL: source pairing, not a native Gallium migration

The current `external/mesa` gitlink carries the app-local Windows ARM64
OpenGL candidate and Zink last-pipeline collision fix. Its exact commit must
match `viogpu/package/verify_bundle.py`'s `D3D10_MESA` identity. The new
`VIOGPU OpenGL source pairing` workflow tests that checkout, not an independently
moving branch. Existing lifetime and KMD recovery gates remain enabled.

## Implemented route

```
OpenGL -> Mesa WGL/state tracker -> Zink -> Vulkan -> Turnip/WDDM -> VIOGPU KMD
```

The Mesa workflow `.github/workflows/windows-zink-turnip-opengl-arm64.yml`
builds `opengl32.dll`, `libgallium_wgl.dll`, `vulkan_freedreno.dll`, `z-1.dll`
and a render/readback probe in one source revision. The resulting artifact is
`droidvm-opengl-zink-turnip-arm64-<mesa SHA>`. It is separate from the signed
flat DriverStore installer. Nothing in this pairing registers the candidate,
changes System32 or DriverStore, or silently replaces the installed OpenGL ICD.
A compatible installed VIOGPU KMD and ARM64 Vulkan loader are prerequisites.

The artifact's README and `run-probe.ps1` describe the explicit, bounded target
test. The launcher confines overrides to its child. The probe requires an
actual Zink-over-Turnip renderer and checks clears, triangle pixels and swaps;
a software fallback is not a PASS. Record the actual GPU model. CI compiles
and validates bytes only; it does not execute that GPU test.

This is plain ARM64, not ARM64EC/ARM64X or x86/x64 game support. Keep it in an
isolated directory on a recoverable test guest; do not mix it into an installed
driver package. No shader version override or deferred-BO opt-in is required.

## Corrected native-driver assessment

Mesa at 241131efe0daea2157e67a2f018feb5104f754fa already has native Gallium
A8xx paths: `freedreno_screen.c` sends generation 8 through `fd6_screen_init`,
and the `a6xx` directory contains generation-specific code. Thus the absence
of a directory named `a8xx` was not evidence that A8xx Gallium was absent.

The actual gap for this Windows guest is that Gallium Freedreno has no matching
WDDM device/pipe/BO/submit/fence backend or WGL integration. Meson expressly
rejects that combination. Native migration remains unfinished; the rejection
and all existing residency/reset/host-detach contracts stay intact. No A8xx
image/SSBO/UBWC or shader-lowering capability is advertised based on a DLL build.

The Zink change revalidates same-hash last-pipeline candidates with the existing
semantic comparator. It closes a correctness risk, not a measured performance
win. The extracted test uses fake keys and the complete driver is compiled by
the separate ARM64 gate. Actual frame time, conformance, reset/TDR and workload
acceptance still require target-device evidence.
