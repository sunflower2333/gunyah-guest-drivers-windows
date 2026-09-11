# Native D3D12 adapter and device lifecycle candidate

This independent candidate preserves ae36d7b's GL recovery, Mesa668d598,
OpenCL distribution and WDDM1.2 implementation. Version 58450 is reserved for
the joint package. The vkd3d engine is pinned in external/vkd3d-proton and built
for ARM64, x64 and x86 in the same workflow as the signed KMD/Mesa pair.

The engine now exports the genuine WDK OpenAdapter12 and constructs a native
device in the runtime's private slot. It retains original callbacks, KMD LUID
and reset generation, selects the unique matching Turnip Vulkan device, and
preserves backend/module lifetime through device and child-object teardown.
The adapter private reply uses the existing producer in
viogpu/shared/viogpu_adapter_identity.h; no new KMD private ABI is introduced.

The incomplete graphics/allocation/residency/fence/Present contract is not
advertised. GetSupportedVersions returns zero, GetCaps refuses unsupported
caps and FillDDITable refuses partial tables. The active UserModeDriverName
registration still selects Mesa. No D3D12 runtime activation is claimed.

Joint output out/candidates/vkd3d contains all three signed engine/fixture
architectures and a signed catalog. package-binding.json ties the sidecar to
the exact parent/Mesa/engine sources and signed KMD/Mesa hashes. The packaging
runner executes each architecture's actual DLL ABI test and controlled WDK
lifecycle fixture, including ARM64. These are not the ordinary system D3D12
application probe or VIOGPU GPU acceptance. Main owns eventual runtime testing
with the independent f6da604 system-runtime probe after required APIs exist.

Still required: complete native device/core tables and feature levels; graphics
pipeline/shaders/draw; allocation/heap backing, GPUVA and residency; monitored
fences; shared surfaces and Present; device removal/reset; and ARM64X/EC front
module integration for native ARM64 and emulated x64 registration.
